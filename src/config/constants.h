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
constexpr uint8_t kIcmSensorCount = 3;

constexpr uint8_t kBmp0Channel = 2;
constexpr uint8_t kBmp1Channel = 3;
constexpr uint8_t kBmpCandidateAddresses[] = {0x77, 0x76};
constexpr uint8_t kBmpExpectedChipId = 0x60;
constexpr uint32_t kBmpSampleRateHz = 25;

// Try the Adafruit STEMMA QT default first, then the alternate address.
constexpr uint8_t kIcmCandidateAddresses[] = {0x69, 0x68};
constexpr uint8_t kIcmWhoAmIRegister = 0x00;
constexpr uint8_t kIcmRegisterBankSelect = 0x7F;
constexpr uint8_t kIcmExpectedWhoAmI = 0xEA;
constexpr uint32_t kMagSampleRateHz = 20;

// Internal sensor ODRs: accel ~=102.3 Hz and gyro =100 Hz.
constexpr uint16_t kIcmAccelRateDivisor = 10;
constexpr uint8_t kIcmGyroRateDivisor = 10;

constexpr uint32_t kImuSampleRateHz = 100;
constexpr uint32_t kImuSamplePeriodUs = 1000000UL / kImuSampleRateHz;
constexpr uint32_t kBmpSamplesPerImuSample =
    kImuSampleRateHz / kBmpSampleRateHz;
constexpr uint32_t kMagSamplesPerImuSample =
    kImuSampleRateHz / kMagSampleRateHz;
static_assert(kImuSampleRateHz % kBmpSampleRateHz == 0,
              "BMP rate must divide IMU rate");
static_assert(kImuSampleRateHz % kMagSampleRateHz == 0,
              "magnetometer rate must divide IMU rate");
static_assert(kMagSamplesPerImuSample >= kIcmSensorCount,
              "magnetometer schedule needs one phase per ICM");
static_assert(1000000UL % kImuSampleRateHz == 0,
              "IMU sample period must be an integer number of microseconds");

constexpr uint16_t kImuPacketMagic = 0xAA55;
constexpr uint8_t kImuPacketVersion = 2;
constexpr uint8_t kCrc8Polynomial = 0x07;

}  // namespace pet::config
