#pragma once

#include <Arduino.h>

namespace pet::pins {

#if defined(CONFIG_IDF_TARGET_ESP32S3)

// OEM ESP32-S3 Super Mini / FH4R2. GPIO19/20 remain reserved for native USB.
constexpr uint8_t kI2cSda = 1;
constexpr uint8_t kI2cScl = 2;
constexpr uint8_t kSdSck = 7;
constexpr uint8_t kSdMiso = 6;
constexpr uint8_t kSdMosi = 5;
constexpr uint8_t kSdChipSelect = 4;
constexpr uint8_t kRgbLedData = 48;

// No microphone pin assignment in the ESP32 stage: audio is suspended.

#else

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

#endif

}  // namespace pet::pins
