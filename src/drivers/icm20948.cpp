#include "drivers/icm20948.h"

#include <math.h>

#include "config/constants.h"

namespace pet::drivers {
namespace {

constexpr uint8_t kAccelDataRegister = 0x2D;
constexpr size_t kAccelGyroDataLength = 12;
constexpr uint8_t kExternalSensorDataRegister = 0x3B;
constexpr size_t kMagnetometerProxyDataLength = 9;
constexpr uint8_t kAk09916Address = 0x0C;
constexpr uint8_t kAk09916WhoAmIRegister = 0x01;
constexpr uint8_t kAk09916ExpectedWhoAmI = 0x09;
constexpr uint8_t kBank0 = 0x00;
constexpr uint8_t kBank3 = 0x30;
constexpr uint8_t kI2cMasterStatusRegister = 0x17;
constexpr uint8_t kI2cSlave4AddressRegister = 0x13;
constexpr uint8_t kI2cSlave4RegisterRegister = 0x14;
constexpr uint8_t kI2cSlave4ControlRegister = 0x15;
constexpr uint8_t kI2cSlave4DataInRegister = 0x17;
constexpr uint8_t kI2cSlave4DoneMask = 0x40;
constexpr uint8_t kI2cSlaveEnable = 0x80;
constexpr uint8_t kAuxiliaryReadBit = 0x80;
constexpr uint8_t kAk09916DataReadyMask = 0x01;
constexpr uint8_t kAk09916DataOverrunMask = 0x02;
constexpr uint8_t kAk09916OverflowMask = 0x08;
constexpr uint8_t kAuxiliaryTransactionPollLimit = 100;
constexpr float kAccelScaleMps2PerCount = (2.0f * 9.80665f) / 32767.5f;
constexpr float kGyroScaleDpsPerCount = 250.0f / 32768.0f;

int16_t decodeBigEndianInt16(const uint8_t* bytes) {
  return static_cast<int16_t>((static_cast<uint16_t>(bytes[0]) << 8) |
                              static_cast<uint16_t>(bytes[1]));
}

int16_t decodeLittleEndianInt16(const uint8_t* bytes) {
  return static_cast<int16_t>(static_cast<uint16_t>(bytes[0]) |
                              (static_cast<uint16_t>(bytes[1]) << 8));
}

bool isFinite(const Vector3f& value) {
  return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

}  // namespace

Icm20948::Icm20948(Pca9548a& mux, TwoWire& wire, uint8_t muxChannel,
                   int32_t sensorId)
    : mux_(mux),
      wire_(wire),
      muxChannel_(muxChannel),
      sensorId_(sensorId) {}

bool Icm20948::begin() {
  initialized_ = false;
  magnetometerWhoAmI_ = 0;

  if (!detectAddress() || !mux_.selectChannel(muxChannel_)) {
    return false;
  }

  if (!sensor_.begin_I2C(address_, &wire_, sensorId_)) {
    return false;
  }

  if (!configure()) {
    return false;
  }

  if (!readAuxiliaryRegister(kAk09916Address, kAk09916WhoAmIRegister,
                             magnetometerWhoAmI_) ||
      magnetometerWhoAmI_ != kAk09916ExpectedWhoAmI) {
    return false;
  }

  uint8_t verifiedWhoAmI = 0;
  if (!readWhoAmI(address_, verifiedWhoAmI) ||
      verifiedWhoAmI != config::kIcmExpectedWhoAmI) {
    return false;
  }

  whoAmI_ = verifiedWhoAmI;
  initialized_ = true;
  return true;
}

bool Icm20948::read(Icm20948Sample& sample) {
  Icm20948RawSample raw{};
  if (!readRaw(raw)) {
    return false;
  }

  sample.timestampUs = micros();
  sample.accelerationMps2 = {
      raw.acceleration.x * kAccelScaleMps2PerCount,
      raw.acceleration.y * kAccelScaleMps2PerCount,
      raw.acceleration.z * kAccelScaleMps2PerCount};
  sample.gyroDps = {raw.gyro.x * kGyroScaleDpsPerCount,
                    raw.gyro.y * kGyroScaleDpsPerCount,
                    raw.gyro.z * kGyroScaleDpsPerCount};

  return isFinite(sample.accelerationMps2) && isFinite(sample.gyroDps);
}

bool Icm20948::readRaw(Icm20948RawSample& sample) {
  if (!initialized_ || !mux_.selectChannel(muxChannel_)) {
    return false;
  }

  if (!writeRegister(address_, config::kIcmRegisterBankSelect, 0x00)) {
    return false;
  }

  uint8_t data[kAccelGyroDataLength]{};
  if (!readRegisters(address_, kAccelDataRegister, data, sizeof(data))) {
    return false;
  }

  sample.acceleration = {decodeBigEndianInt16(&data[0]),
                         decodeBigEndianInt16(&data[2]),
                         decodeBigEndianInt16(&data[4])};
  sample.gyro = {decodeBigEndianInt16(&data[6]),
                 decodeBigEndianInt16(&data[8]),
                 decodeBigEndianInt16(&data[10])};
  return true;
}

bool Icm20948::readMagnetometerRaw(Ak09916RawSample& sample) {
  if (!initialized_ || !mux_.selectChannel(muxChannel_) ||
      !writeRegister(address_, config::kIcmRegisterBankSelect, kBank0)) {
    return false;
  }

  uint8_t data[kMagnetometerProxyDataLength]{};
  if (!readRegisters(address_, kExternalSensorDataRegister, data,
                     sizeof(data))) {
    return false;
  }

  sample.status1 = data[0];
  sample.magnetic = {decodeLittleEndianInt16(&data[1]),
                     decodeLittleEndianInt16(&data[3]),
                     decodeLittleEndianInt16(&data[5])};
  sample.status2 = data[8];
  sample.dataReady = (sample.status1 & kAk09916DataReadyMask) != 0;
  sample.dataOverrun = (sample.status1 & kAk09916DataOverrunMask) != 0;
  sample.overflow = (sample.status2 & kAk09916OverflowMask) != 0;
  return true;
}

bool Icm20948::detectAddress() {
  address_ = 0;
  whoAmI_ = 0;

  if (!mux_.selectChannel(muxChannel_)) {
    return false;
  }

  for (const uint8_t candidate : config::kIcmCandidateAddresses) {
    uint8_t value = 0;
    if (readWhoAmI(candidate, value) &&
        value == config::kIcmExpectedWhoAmI) {
      address_ = candidate;
      whoAmI_ = value;
      return true;
    }
  }

  return false;
}

bool Icm20948::readWhoAmI(uint8_t address, uint8_t& value) {
  if (!writeRegister(address, config::kIcmRegisterBankSelect, 0x00)) {
    return false;
  }
  return readRegister(address, config::kIcmWhoAmIRegister, value);
}

bool Icm20948::writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  wire_.beginTransmission(address);
  wire_.write(reg);
  wire_.write(value);
  return wire_.endTransmission() == 0;
}

bool Icm20948::readRegister(uint8_t address, uint8_t reg, uint8_t& value) {
  return readRegisters(address, reg, &value, 1);
}

bool Icm20948::readRegisters(uint8_t address, uint8_t startRegister,
                             uint8_t* data, size_t length) {
  if (data == nullptr || length == 0 || length > 255) {
    return false;
  }

  wire_.beginTransmission(address);
  wire_.write(startRegister);
  if (wire_.endTransmission(false) != 0) {
    return false;
  }

  const uint8_t requested = static_cast<uint8_t>(length);
  if (wire_.requestFrom(address, requested) != requested) {
    return false;
  }

  for (size_t i = 0; i < length; ++i) {
    if (!wire_.available()) {
      return false;
    }
    data[i] = static_cast<uint8_t>(wire_.read());
  }
  return true;
}

bool Icm20948::readAuxiliaryRegister(uint8_t slaveAddress,
                                     uint8_t registerAddress,
                                     uint8_t& value) {
  if (!mux_.selectChannel(muxChannel_) ||
      !writeRegister(address_, config::kIcmRegisterBankSelect, kBank3) ||
      !writeRegister(address_, kI2cSlave4AddressRegister,
                     slaveAddress | kAuxiliaryReadBit) ||
      !writeRegister(address_, kI2cSlave4RegisterRegister, registerAddress) ||
      !writeRegister(address_, kI2cSlave4ControlRegister, kI2cSlaveEnable) ||
      !writeRegister(address_, config::kIcmRegisterBankSelect, kBank0)) {
    return false;
  }

  bool transactionFinished = false;
  for (uint8_t attempt = 0; attempt < kAuxiliaryTransactionPollLimit;
       ++attempt) {
    uint8_t status = 0;
    if (!readRegister(address_, kI2cMasterStatusRegister, status)) {
      return false;
    }
    if ((status & kI2cSlave4DoneMask) != 0) {
      transactionFinished = true;
      break;
    }
  }
  if (!transactionFinished ||
      !writeRegister(address_, config::kIcmRegisterBankSelect, kBank3) ||
      !readRegister(address_, kI2cSlave4DataInRegister, value)) {
    return false;
  }
  return writeRegister(address_, config::kIcmRegisterBankSelect, kBank0);
}

bool Icm20948::configure() {
  sensor_.setAccelRange(ICM20948_ACCEL_RANGE_2_G);
  sensor_.setGyroRange(ICM20948_GYRO_RANGE_250_DPS);
  sensor_.setAccelRateDivisor(config::kIcmAccelRateDivisor);
  sensor_.setGyroRateDivisor(config::kIcmGyroRateDivisor);

  const bool accelFilterDisabled =
      sensor_.enableAccelDLPF(false, ICM20X_ACCEL_FREQ_50_4_HZ);
  const bool gyroFilterDisabled =
      sensor_.enableGyrolDLPF(false, ICM20X_GYRO_FREQ_51_2_HZ);
  const bool magnetometerConfigured =
      sensor_.setMagDataRate(AK09916_MAG_DATARATE_20_HZ);

  return accelFilterDisabled && gyroFilterDisabled && magnetometerConfigured &&
         sensor_.getAccelRange() == ICM20948_ACCEL_RANGE_2_G &&
         sensor_.getGyroRange() == ICM20948_GYRO_RANGE_250_DPS &&
         sensor_.getAccelRateDivisor() == config::kIcmAccelRateDivisor &&
         sensor_.getGyroRateDivisor() == config::kIcmGyroRateDivisor;
}

}  // namespace pet::drivers
