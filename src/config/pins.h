#pragma once

#include <Arduino.h>

namespace pet::pins {

// I2C0 / Wire - PCA9548A upstream bus.
constexpr uint8_t kI2cSda = 18;
constexpr uint8_t kI2cScl = 19;

// SPI microSD (reserved for the SD logging stage).
constexpr uint8_t kSdMosi = 11;
constexpr uint8_t kSdMiso = 12;
constexpr uint8_t kSdSck = 13;
constexpr uint8_t kSdChipSelect = 10;

// I2S microphone (reserved for the audio stage).
constexpr uint8_t kMicBclk = 21;
constexpr uint8_t kMicLrclk = 20;
constexpr uint8_t kMicData = 8;

}  // namespace pet::pins
