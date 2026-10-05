#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>
#include <Wire.h>

#include "config/pins.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "This preparation entry point requires ESP32-S3"
#endif

namespace {

constexpr uint32_t kSerialBaud = 115200;
constexpr uint32_t kI2cClockHz = 400000;
constexpr uint32_t kReportIntervalMs = 5000;
constexpr uint32_t kExpectedFlashBytes = 4U * 1024U * 1024U;
constexpr uint32_t kExpectedPsramBytes = 2U * 1024U * 1024U;

bool i2cReady = false;
uint32_t lastReportMs = 0;

void reportConfiguration() {
  const uint32_t flashBytes = ESP.getFlashChipSize();
  const uint32_t psramBytes = ESP.getPsramSize();
  Serial.printf("PBIC_STAGE=esp32s3_preparation cpu_mhz=%u flash_bytes=%lu "
                "psram_bytes=%lu memory_config_ok=%u\n",
                ESP.getCpuFreqMHz(), static_cast<unsigned long>(flashBytes),
                static_cast<unsigned long>(psramBytes),
                flashBytes == kExpectedFlashBytes &&
                        psramBytes == kExpectedPsramBytes
                    ? 1U
                    : 0U);
  Serial.printf("I2C_INIT ok=%u sda=%u scl=%u clock_hz=%lu\n",
                i2cReady ? 1U : 0U, pet::pins::kI2cSda, pet::pins::kI2cScl,
                static_cast<unsigned long>(kI2cClockHz));
  Serial.printf("SD_PREPARED library=SdFat type=SdFs type_bytes=%u "
                "sck=%u miso=%u mosi=%u cs=%u mounted=0\n",
                static_cast<unsigned>(sizeof(SdFs)), pet::pins::kSdSck,
                pet::pins::kSdMiso, pet::pins::kSdMosi, pet::pins::kSdChipSelect);
  Serial.println("ACQUISITION_ENABLED=0 AUDIO_ENABLED=0 SD_WRITES_ENABLED=0");
}

}  // namespace

void setup() {
  Serial.begin(kSerialBaud);

  pinMode(pet::pins::kSdChipSelect, OUTPUT);
  digitalWrite(pet::pins::kSdChipSelect, HIGH);
  SPI.begin(pet::pins::kSdSck, pet::pins::kSdMiso, pet::pins::kSdMosi,
            pet::pins::kSdChipSelect);
  i2cReady = Wire.begin(pet::pins::kI2cSda, pet::pins::kI2cScl, kI2cClockHz);

  reportConfiguration();
  lastReportMs = millis();
}

void loop() {
  const uint32_t nowMs = millis();
  if (nowMs - lastReportMs >= kReportIntervalMs) {
    lastReportMs = nowMs;
    reportConfiguration();
  }
  delay(1);
}
