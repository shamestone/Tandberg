#pragma once

#include <Arduino.h>

// Tandberg V8 final PCB pin map, confirmed against FIRMWARE_PINMAP.txt and PCB U1 nets.
constexpr uint8_t PIN_AUDIO_ADC = 3;      // XIAO D1 / GPIO3
constexpr uint8_t PIN_LED_DIM_ADC = 4;    // XIAO D2 / GPIO4
constexpr uint8_t PIN_EYE_RELAY = 5;      // XIAO D3 / GPIO5, HIGH energizes relay
constexpr uint8_t PIN_RADIO_POWER_N = 6;  // XIAO D4 / GPIO6, active-LOW H11AA1 pulses
constexpr uint8_t PIN_ANY_LAMP_N = 7;     // XIAO D5 / GPIO7, active-LOW H11AA1 pulses
constexpr uint8_t PIN_LED_PWM = 21;       // XIAO D6 / GPIO21, active-high low-side MOSFET
constexpr uint8_t PIN_EYE_PWM = 20;       // XIAO D7 / GPIO20, active-high H11AA1 drive

// Intentionally unused: GPIO2/D0, GPIO8/D8, GPIO9/D9/BOOT, GPIO10/D10.
