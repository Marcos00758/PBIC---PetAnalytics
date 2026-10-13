#include <Arduino.h>
#include <Wire.h>

#include "config/esp32_runtime.h"
#include "config/pins.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "The PCA diagnostic requires ESP32-S3"
#endif

namespace {
constexpr uint32_t kClockRatesHz[] = {100000, 400000};
bool i2cReady = false;
uint32_t lastTestMs = 0;

void testFrequency(uint32_t clockHz) {
  const bool clockReady = Wire.setClock(clockHz);
  Serial.printf("PCA_SCAN_BEGIN hz=%lu clock_ok=%u\n",
                static_cast<unsigned long>(clockHz), clockReady);
  if (!clockReady) return;

  unsigned responses = 0, successfulWrites = 0;
  for (uint8_t address = 0x70; address <= 0x77; ++address) {
    Wire.beginTransmission(address);
    const uint8_t probeCode = Wire.endTransmission();
    Serial.printf("PCA_RESULT hz=%lu address=0x%02x op=probe code=%u\n",
                  static_cast<unsigned long>(clockHz), address, probeCode);
    if (probeCode != 0) {
      Serial.printf("PCA_WRITE_SKIPPED address=0x%02x reason=probe_failed\n", address);
      continue;
    }

    ++responses;
    Wire.beginTransmission(address);
    Wire.write(uint8_t(0x00));  // PCA control byte: deselect every channel.
    const uint8_t writeCode = Wire.endTransmission();
    Serial.printf("PCA_RESULT hz=%lu address=0x%02x op=write_zero code=%u\n",
                  static_cast<unsigned long>(clockHz), address, writeCode);
    if (writeCode == 0) ++successfulWrites;
  }
  Serial.printf("PCA_SCAN_END hz=%lu responses=%u writes_ok=%u\n",
                static_cast<unsigned long>(clockHz), responses, successfulWrites);
}

void runTests() {
  Serial.printf("PBIC_STAGE=esp32s3_pca_test version=1 i2c_ready=%u "
                "sda=%u scl=%u timeout_ms=%lu\n", i2cReady,
                pet::pins::kI2cSda, pet::pins::kI2cScl,
                static_cast<unsigned long>(pet::config::kI2cTimeoutMs));
  if (!i2cReady) return;
  for (const uint32_t clockHz : kClockRatesHz) testFrequency(clockHz);
}
}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t startMs = millis();
  while (!Serial && millis() - startMs < 1500) delay(10);

  // Keep the attached SD card deselected without initializing its SPI bus.
  pinMode(pet::pins::kSdChipSelect, OUTPUT);
  digitalWrite(pet::pins::kSdChipSelect, HIGH);
  i2cReady = Wire.begin(pet::pins::kI2cSda, pet::pins::kI2cScl, kClockRatesHz[0]);
  Wire.setTimeOut(pet::config::kI2cTimeoutMs);
  runTests();
  lastTestMs = millis();
}

void loop() {
  if (millis() - lastTestMs >= pet::config::kRuntimeReportMs) {
    runTests();
    lastTestMs = millis();
  }
  delay(10);
}
