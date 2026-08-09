#pragma once

#include <Arduino.h>

namespace pet::config {

constexpr uint32_t kSerialBaud = 115200;
constexpr uint32_t kSerialWaitTimeoutMs = 3000;
constexpr uint32_t kI2cClockHz = 400000;

constexpr uint8_t kPca9548aAddress = 0x70;
constexpr uint16_t kPcaChannelSettleUs = 80;

constexpr uint8_t kIcm0Channel = 0;
constexpr uint8_t kIcm1Channel = 1;
constexpr uint8_t kIcm2Channel = 4;

constexpr uint8_t kBmp0Channel = 2;
constexpr uint8_t kBmp1Channel = 3;
constexpr uint8_t kBmpCandidateAddresses[] = {0x77, 0x76};
constexpr uint8_t kBmpExpectedChipId = 0x60;
constexpr uint16_t kBmpDiagnosticRounds = 100;
constexpr uint16_t kBmpDiagnosticWarmupRounds = 10;
constexpr uint32_t kBmpDiagnosticPeriodUs = 100000;

// Try the Adafruit STEMMA QT default first, then the alternate address.
constexpr uint8_t kIcmCandidateAddresses[] = {0x69, 0x68};
constexpr uint8_t kIcmWhoAmIRegister = 0x00;
constexpr uint8_t kIcmRegisterBankSelect = 0x7F;
constexpr uint8_t kIcmExpectedWhoAmI = 0xEA;

// Internal sensor ODRs: accel ~=102.3 Hz and gyro =100 Hz.
constexpr uint16_t kIcmAccelRateDivisor = 10;
constexpr uint8_t kIcmGyroRateDivisor = 10;

constexpr uint32_t kImuSampleRateHz = 100;
constexpr uint32_t kImuSamplePeriodUs = 1000000UL / kImuSampleRateHz;
static_assert(1000000UL % kImuSampleRateHz == 0,
              "IMU sample period must be an integer number of microseconds");

constexpr uint16_t kImuPacketMagic = 0xAA55;
constexpr uint8_t kCrc8Polynomial = 0x07;

}  // namespace pet::config
