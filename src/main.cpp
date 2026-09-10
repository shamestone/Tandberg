#include <Arduino.h>
#include "pins.h"

enum class AutoMode : uint8_t {
  Off,
  Radio,
  Gram,
};

enum class ControlMode : uint8_t {
  Automatic,
  Manual,
};

// PWM calibration.
constexpr uint32_t LED_PWM_FREQUENCY_HZ = 20000;
constexpr uint32_t EYE_PWM_FREQUENCY_HZ = 1500;
constexpr uint8_t PWM_RESOLUTION_BITS = 10;
constexpr uint16_t PWM_MAX_DUTY = (1U << PWM_RESOLUTION_BITS) - 1U;

// Arduino-ESP32 2.x LEDC channels. These are ignored by the 3.x compatibility path.
constexpr uint8_t LED_PWM_CHANNEL = 0;
constexpr uint8_t EYE_PWM_CHANNEL = 1;

// AC optocoupler pulse tracking. 45 ms bridges 50/60 Hz H11AA1 pulse gaps with margin.
constexpr uint32_t AC_PRESENT_WINDOW_MS = 45;
constexpr uint32_t GRAM_NO_LAMP_QUALIFY_MS = 300;
constexpr uint32_t RADIO_LAMP_HOLD_MS = 120;

// ADC sampling and filtering.
constexpr uint32_t AUDIO_SAMPLE_PERIOD_US = 250;  // 4 kHz audio envelope sampling.
constexpr uint32_t DIM_SAMPLE_PERIOD_MS = 10;
constexpr uint32_t STATUS_PERIOD_MS = 1000;
constexpr uint32_t ADC_MAX_COUNTS = 4095;

// Magic-eye envelope calibration. Raise AUDIO_NOISE_FLOOR_COUNTS if the eye flickers at silence.
constexpr uint16_t AUDIO_NOISE_FLOOR_COUNTS = 18;
constexpr uint16_t AUDIO_FULL_SCALE_COUNTS = 850;
constexpr uint8_t MIDPOINT_SHIFT = 8;      // Slow DC midpoint tracking: 1/256 per sample.
constexpr uint8_t ENVELOPE_ATTACK_SHIFT = 2;   // Fast attack: 1/4 of error per sample.
constexpr uint8_t ENVELOPE_RELEASE_SHIFT = 5;  // Slower release: 1/32 of error per sample.

// LED dimmer filtering and simple perceptual response.
constexpr uint8_t LED_DIM_FILTER_SHIFT = 4;  // 1/16 low-pass on RV1.
constexpr uint8_t LED_GAMMA_NUMERATOR = 2;   // duty = percent^2 / 100 for smoother low end.

struct Diagnostics {
  int audioRaw = 0;
  int ledDimRaw = 0;
  uint16_t ledDimFiltered = 0;
  int32_t audioMidpoint = ADC_MAX_COUNTS / 2;
  uint16_t audioAmplitude = 0;
  uint16_t audioEnvelope = 0;
  uint16_t ledDuty = 0;
  uint16_t eyeDuty = 0;
  bool relay = false;
};

ControlMode controlMode = ControlMode::Automatic;
AutoMode currentAutoMode = AutoMode::Off;
AutoMode previousAutoMode = AutoMode::Off;
Diagnostics diag;

uint32_t lastRadioLowMs = 0;
uint32_t lastLampLowMs = 0;
uint32_t lastAudioSampleUs = 0;
uint32_t lastDimSampleMs = 0;
uint32_t lastStatusMs = 0;
String serialLine;

uint16_t manualLedDuty = 0;
uint16_t manualEyeDuty = 0;
bool manualRelay = false;

const char *modeName(AutoMode mode) {
  switch (mode) {
    case AutoMode::Off:
      return "OFF";
    case AutoMode::Radio:
      return "RADIO";
    case AutoMode::Gram:
      return "GRAM";
  }
  return "?";
}

uint32_t elapsedMs(uint32_t now, uint32_t then) {
  return now - then;
}

uint16_t percentToDuty(int percent) {
  percent = constrain(percent, 0, 100);
  return static_cast<uint16_t>((static_cast<uint32_t>(percent) * PWM_MAX_DUTY + 50U) / 100U);
}

uint16_t envelopeToDuty(uint16_t envelope) {
  if (envelope <= AUDIO_NOISE_FLOOR_COUNTS) {
    return 0;
  }

  const uint32_t span = max<uint16_t>(1, AUDIO_FULL_SCALE_COUNTS - AUDIO_NOISE_FLOOR_COUNTS);
  const uint32_t active = min<uint32_t>(envelope - AUDIO_NOISE_FLOOR_COUNTS, span);
  return static_cast<uint16_t>((active * PWM_MAX_DUTY) / span);
}

uint16_t ledAdcToDuty(uint16_t filteredAdc) {
  const uint32_t percent = (static_cast<uint32_t>(filteredAdc) * 100U + (ADC_MAX_COUNTS / 2U)) / ADC_MAX_COUNTS;
  const uint32_t gammaPercent = (percent * percent + 50U) / 100U;
  (void)LED_GAMMA_NUMERATOR;
  return percentToDuty(static_cast<int>(gammaPercent));
}

void pwmWriteLed(uint16_t duty) {
  duty = min<uint16_t>(duty, PWM_MAX_DUTY);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_LED_PWM, duty);
#else
  ledcWrite(LED_PWM_CHANNEL, duty);
#endif
  diag.ledDuty = duty;
}

void pwmWriteEye(uint16_t duty) {
  duty = min<uint16_t>(duty, PWM_MAX_DUTY);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_EYE_PWM, duty);
#else
  ledcWrite(EYE_PWM_CHANNEL, duty);
#endif
  diag.eyeDuty = duty;
}

void setRelay(bool energized) {
  digitalWrite(PIN_EYE_RELAY, energized ? HIGH : LOW);
  diag.relay = energized;
}

void applyOutputs(uint16_t ledDuty, uint16_t eyeDuty, bool relay) {
  pwmWriteLed(ledDuty);
  pwmWriteEye(eyeDuty);
  setRelay(relay);
}

void configurePwm() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_LED_PWM, LED_PWM_FREQUENCY_HZ, PWM_RESOLUTION_BITS);
  ledcAttach(PIN_EYE_PWM, EYE_PWM_FREQUENCY_HZ, PWM_RESOLUTION_BITS);
#else
  ledcSetup(LED_PWM_CHANNEL, LED_PWM_FREQUENCY_HZ, PWM_RESOLUTION_BITS);
  ledcSetup(EYE_PWM_CHANNEL, EYE_PWM_FREQUENCY_HZ, PWM_RESOLUTION_BITS);
  ledcAttachPin(PIN_LED_PWM, LED_PWM_CHANNEL);
  ledcAttachPin(PIN_EYE_PWM, EYE_PWM_CHANNEL);
#endif
  pwmWriteLed(0);
  pwmWriteEye(0);
}

void samplePulseInputs(uint32_t nowMs) {
  if (digitalRead(PIN_RADIO_POWER_N) == LOW) {
    lastRadioLowMs = nowMs;
  }
  if (digitalRead(PIN_ANY_LAMP_N) == LOW) {
    lastLampLowMs = nowMs;
  }
}

bool radioPresent(uint32_t nowMs) {
  return elapsedMs(nowMs, lastRadioLowMs) <= AC_PRESENT_WINDOW_MS;
}

bool lampRecent(uint32_t nowMs, uint32_t holdMs) {
  return elapsedMs(nowMs, lastLampLowMs) <= holdMs;
}

void sampleAudio(uint32_t nowUs) {
  if (nowUs - lastAudioSampleUs < AUDIO_SAMPLE_PERIOD_US) {
    return;
  }
  lastAudioSampleUs += AUDIO_SAMPLE_PERIOD_US;

  const int raw = analogRead(PIN_AUDIO_ADC);
  diag.audioRaw = raw;

  diag.audioMidpoint += (raw - diag.audioMidpoint) >> MIDPOINT_SHIFT;
  const uint16_t amplitude = abs(raw - diag.audioMidpoint);
  diag.audioAmplitude = amplitude;

  if (amplitude > diag.audioEnvelope) {
    diag.audioEnvelope += (amplitude - diag.audioEnvelope) >> ENVELOPE_ATTACK_SHIFT;
  } else {
    diag.audioEnvelope -= (diag.audioEnvelope - amplitude) >> ENVELOPE_RELEASE_SHIFT;
  }
}

void sampleLedDim(uint32_t nowMs) {
  if (nowMs - lastDimSampleMs < DIM_SAMPLE_PERIOD_MS) {
    return;
  }
  lastDimSampleMs = nowMs;

  const int raw = analogRead(PIN_LED_DIM_ADC);
  diag.ledDimRaw = raw;
  diag.ledDimFiltered += (raw - diag.ledDimFiltered) >> LED_DIM_FILTER_SHIFT;
}

AutoMode calculateAutoMode(uint32_t nowMs) {
  if (!radioPresent(nowMs)) {
    return AutoMode::Off;
  }

  if (lampRecent(nowMs, AC_PRESENT_WINDOW_MS)) {
    return AutoMode::Radio;
  }

  if (previousAutoMode == AutoMode::Radio && lampRecent(nowMs, RADIO_LAMP_HOLD_MS)) {
    return AutoMode::Radio;
  }

  if (!lampRecent(nowMs, GRAM_NO_LAMP_QUALIFY_MS)) {
    return AutoMode::Gram;
  }

  return previousAutoMode == AutoMode::Gram ? AutoMode::Gram : AutoMode::Radio;
}

void runAutomatic(uint32_t nowMs) {
  currentAutoMode = calculateAutoMode(nowMs);

  switch (currentAutoMode) {
    case AutoMode::Off:
      applyOutputs(0, 0, false);
      break;
    case AutoMode::Radio:
      applyOutputs(0, 0, false);
      break;
    case AutoMode::Gram:
      applyOutputs(ledAdcToDuty(diag.ledDimFiltered), envelopeToDuty(diag.audioEnvelope), true);
      break;
  }

  previousAutoMode = currentAutoMode;
}

void printStatus(uint32_t nowMs) {
  Serial.print(F("mode="));
  Serial.print(modeName(currentAutoMode));
  Serial.print(F(" control="));
  Serial.print(controlMode == ControlMode::Automatic ? F("auto") : F("manual"));
  Serial.print(F(" radio_n="));
  Serial.print(digitalRead(PIN_RADIO_POWER_N) == LOW ? F("LOW") : F("HIGH"));
  Serial.print(F(" radio_low_age_ms="));
  Serial.print(elapsedMs(nowMs, lastRadioLowMs));
  Serial.print(F(" lamp_n="));
  Serial.print(digitalRead(PIN_ANY_LAMP_N) == LOW ? F("LOW") : F("HIGH"));
  Serial.print(F(" lamp_low_age_ms="));
  Serial.print(elapsedMs(nowMs, lastLampLowMs));
  Serial.print(F(" led_dim_raw="));
  Serial.print(diag.ledDimRaw);
  Serial.print(F(" led_dim_filtered="));
  Serial.print(diag.ledDimFiltered);
  Serial.print(F(" audio_raw="));
  Serial.print(diag.audioRaw);
  Serial.print(F(" audio_midpoint="));
  Serial.print(diag.audioMidpoint);
  Serial.print(F(" audio_amp="));
  Serial.print(diag.audioAmplitude);
  Serial.print(F(" audio_env="));
  Serial.print(diag.audioEnvelope);
  Serial.print(F(" led_duty="));
  Serial.print(diag.ledDuty);
  Serial.print(F(" eye_duty="));
  Serial.print(diag.eyeDuty);
  Serial.print(F(" relay="));
  Serial.println(diag.relay ? F("HIGH") : F("LOW"));
}

void printHelp() {
  Serial.println(F("Commands: status, auto, mode off, mode radio, mode gram, led 0..100, eye 0..100, relay 0|1"));
}

void enterManualOff() {
  controlMode = ControlMode::Manual;
  currentAutoMode = AutoMode::Off;
  manualLedDuty = 0;
  manualEyeDuty = 0;
  manualRelay = false;
  applyOutputs(manualLedDuty, manualEyeDuty, manualRelay);
}

void enterManualRadio() {
  controlMode = ControlMode::Manual;
  currentAutoMode = AutoMode::Radio;
  manualLedDuty = 0;
  manualEyeDuty = 0;
  manualRelay = false;
  applyOutputs(manualLedDuty, manualEyeDuty, manualRelay);
}

void enterManualGram() {
  controlMode = ControlMode::Manual;
  currentAutoMode = AutoMode::Gram;
  manualLedDuty = ledAdcToDuty(diag.ledDimFiltered);
  manualEyeDuty = envelopeToDuty(diag.audioEnvelope);
  manualRelay = true;
  applyOutputs(manualLedDuty, manualEyeDuty, manualRelay);
}

void handleCommand(String command) {
  command.trim();
  command.toLowerCase();

  const uint32_t nowMs = millis();
  if (command.length() == 0) {
    return;
  }
  if (command == F("status")) {
    printStatus(nowMs);
  } else if (command == F("auto")) {
    controlMode = ControlMode::Automatic;
    Serial.println(F("OK auto"));
  } else if (command == F("mode off")) {
    enterManualOff();
    Serial.println(F("OK manual mode off"));
  } else if (command == F("mode radio")) {
    enterManualRadio();
    Serial.println(F("OK manual mode radio"));
  } else if (command == F("mode gram")) {
    enterManualGram();
    Serial.println(F("OK manual mode gram"));
  } else if (command.startsWith(F("led "))) {
    controlMode = ControlMode::Manual;
    manualLedDuty = percentToDuty(command.substring(4).toInt());
    applyOutputs(manualLedDuty, manualEyeDuty, manualRelay);
    Serial.println(F("OK manual led"));
  } else if (command.startsWith(F("eye "))) {
    controlMode = ControlMode::Manual;
    manualEyeDuty = percentToDuty(command.substring(4).toInt());
    applyOutputs(manualLedDuty, manualEyeDuty, manualRelay);
    Serial.println(F("OK manual eye"));
  } else if (command == F("relay 0")) {
    controlMode = ControlMode::Manual;
    manualRelay = false;
    applyOutputs(manualLedDuty, manualEyeDuty, manualRelay);
    Serial.println(F("OK manual relay 0"));
  } else if (command == F("relay 1")) {
    controlMode = ControlMode::Manual;
    manualRelay = true;
    applyOutputs(manualLedDuty, manualEyeDuty, manualRelay);
    Serial.println(F("OK manual relay 1"));
  } else {
    Serial.println(F("ERR unknown command"));
    printHelp();
  }
}

void pollSerial() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      handleCommand(serialLine);
      serialLine = "";
    } else if (serialLine.length() < 80) {
      serialLine += c;
    }
  }
}

void setup() {
  pinMode(PIN_EYE_RELAY, OUTPUT);
  digitalWrite(PIN_EYE_RELAY, LOW);

  pinMode(PIN_LED_PWM, OUTPUT);
  digitalWrite(PIN_LED_PWM, LOW);
  pinMode(PIN_EYE_PWM, OUTPUT);
  digitalWrite(PIN_EYE_PWM, LOW);

  pinMode(PIN_RADIO_POWER_N, INPUT_PULLUP);
  pinMode(PIN_ANY_LAMP_N, INPUT_PULLUP);
  pinMode(PIN_AUDIO_ADC, INPUT);
  pinMode(PIN_LED_DIM_ADC, INPUT);

  analogReadResolution(12);
  configurePwm();
  applyOutputs(0, 0, false);

  diag.audioRaw = analogRead(PIN_AUDIO_ADC);
  diag.audioMidpoint = diag.audioRaw;
  diag.ledDimRaw = analogRead(PIN_LED_DIM_ADC);
  diag.ledDimFiltered = diag.ledDimRaw;

  lastRadioLowMs = millis() - (AC_PRESENT_WINDOW_MS + 1U);
  lastLampLowMs = millis() - (GRAM_NO_LAMP_QUALIFY_MS + 1U);
  lastAudioSampleUs = micros();
  lastDimSampleMs = millis();
  lastStatusMs = millis();

  Serial.begin(115200);
  Serial.println(F("Tandberg V8 Magic Eye firmware"));
  printHelp();
}

void loop() {
  const uint32_t nowMs = millis();
  const uint32_t nowUs = micros();

  samplePulseInputs(nowMs);
  sampleAudio(nowUs);
  sampleLedDim(nowMs);
  pollSerial();

  if (controlMode == ControlMode::Automatic) {
    runAutomatic(nowMs);
  }

  if (nowMs - lastStatusMs >= STATUS_PERIOD_MS) {
    lastStatusMs = nowMs;
    printStatus(nowMs);
  }
}
