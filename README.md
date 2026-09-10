# Tandberg V8 Magic Eye Firmware

Deterministic standalone firmware for the final Tandberg V8 Magic Eye PCB using a Seeed Studio XIAO ESP32-C3.

The hardware mapping was checked against `FIRMWARE_PINMAP.txt` and the KiCad PCB U1 nets from the supplied hardware ZIP. KiCad files are reference only and are not modified by this firmware project.

## Pin Map

| Function | XIAO pin | ESP32-C3 GPIO |
| --- | --- | --- |
| AUDIO_ADC | D1 | GPIO3 |
| LED_DIM_ADC | D2 | GPIO4 |
| EYE_RELAY | D3 | GPIO5 |
| RADIO_POWER_N | D4 | GPIO6 |
| ANY_LAMP_N | D5 | GPIO7 |
| LED_PWM | D6 | GPIO21 |
| EYE_PWM | D7 | GPIO20 |

Unused by firmware: GPIO2/D0, GPIO8/D8, GPIO9/D9/BOOT, GPIO10/D10.

## Safety Behavior

At boot the firmware immediately drives:

- `EYE_RELAY` LOW, leaving K1 in the original-radio AGC position.
- `LED_PWM` LOW / 0 duty.
- `EYE_PWM` LOW / 0 duty.

If radio-power pulses stop, automatic mode immediately returns outputs toward `OFF`.

## Automatic Modes

- `OFF`: LED off, eye PWM off, relay LOW.
- `RADIO`: radio power pulses are present and a band-lamp pulse was recently seen. LED off, eye PWM off, relay LOW.
- `GRAM`: radio power pulses are present and no band-lamp pulse has been seen for the qualification interval. LED strip follows RV1, relay HIGH, magic eye follows the audio envelope.

`RADIO_POWER_N` and `ANY_LAMP_N` are AC optocoupler pulse signals, not static logic inputs. The firmware tracks the most recent LOW detection and uses age windows/hysteresis.

## Calibration Constants

Main constants live near the top of `src/main.cpp`:

- `AC_PRESENT_WINDOW_MS = 45`: radio/lamp pulse considered present when LOW was observed recently.
- `GRAM_NO_LAMP_QUALIFY_MS = 300`: lamp-free qualification time before entering GRAM.
- `RADIO_LAMP_HOLD_MS = 120`: extra lamp hold time to avoid band-change flicker.
- `AUDIO_SAMPLE_PERIOD_US = 250`: audio envelope sample rate, 4 kHz.
- `AUDIO_NOISE_FLOOR_COUNTS = 18`: envelope below this maps to zero eye PWM.
- `AUDIO_FULL_SCALE_COUNTS = 850`: envelope value that maps to full eye PWM.
- `MIDPOINT_SHIFT = 8`: slow DC midpoint tracking.
- `ENVELOPE_ATTACK_SHIFT = 2`: fast envelope attack.
- `ENVELOPE_RELEASE_SHIFT = 5`: slower envelope release.
- `LED_DIM_FILTER_SHIFT = 4`: RV1 ADC smoothing.

PWM uses 10-bit resolution, 20 kHz for `LED_PWM`, and 1.5 kHz for `EYE_PWM`. Upload speed is set to 115200 baud for reliable first bring-up on the XIAO ESP32-C3 USB serial bridge.

## Serial Diagnostics

Serial monitor speed is 115200 baud. Commands:

```text
status
auto
mode off
mode radio
mode gram
led 0..100
eye 0..100
relay 0
relay 1
```

Manual commands are for bench testing and put the firmware in manual control. Use `auto` to return to the automatic state machine.

The firmware prints periodic status once per second and also prints immediate output for `status`. It does not spam every loop.

## Build

If PlatformIO is installed globally:

```powershell
pio run
```

This workspace was set up to work with a local Python virtual environment. On this machine, `python` was not on `PATH`, so KiCad's bundled Python was used to create `.venv`:

```powershell
& 'C:\Program Files\KiCad\10.0\bin\python.exe' -m venv .venv
.\.venv\Scripts\python -m pip install --upgrade pip platformio
.\.venv\Scripts\pio run
```

## Upload And Monitor

After a clean build, detect connected serial ports:

```powershell
.\.venv\Scripts\pio device list
```

Upload, replacing `COMx` with the detected XIAO ESP32-C3 port:

```powershell
.\.venv\Scripts\pio run -t upload --upload-port COMx
```

For the board detected during project bring-up:

```powershell
.\.venv\Scripts\pio run -t upload --upload-port COM4
```

Open the serial monitor:

```powershell
.\.venv\Scripts\pio device monitor -b 115200 --port COMx
```
