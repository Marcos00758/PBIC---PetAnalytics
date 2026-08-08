#include "drivers/icm20948.h"

#include <math.h>

#include "config/constants.h"

namespace pet::drivers {
namespace {

constexpr float kRadiansToDegrees = 57.29577951308232f;

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

  if (!detectAddress() || !mux_.selectChannel(muxChannel_)) {
    return false;
  }

  if (!sensor_.begin_I2C(address_, &wire_, sensorId_)) {
    return false;
  }

  if (!configure()) {
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
  if (!initialized_ || !mux_.selectChannel(muxChannel_)) {
    return false;
  }

  uint8_t currentWhoAmI = 0;
  if (!readWhoAmI(address_, currentWhoAmI) || currentWhoAmI != whoAmI_) {
    return false;
  }

  sensors_event_t accel{};
  sensors_event_t gyro{};
  sensors_event_t temperature{};
  if (!sensor_.getEvent(&accel, &gyro, &temperature, nullptr)) {
    return false;
  }

  sample.timestampUs = micros();
  sample.accelerationMps2 = {accel.acceleration.x, accel.acceleration.y,
                             accel.acceleration.z};
  sample.gyroDps = {gyro.gyro.x * kRadiansToDegrees,
                    gyro.gyro.y * kRadiansToDegrees,
                    gyro.gyro.z * kRadiansToDegrees};

  return isFinite(sample.accelerationMps2) && isFinite(sample.gyroDps);
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
  wire_.beginTransmission(address);
  wire_.write(reg);
  if (wire_.endTransmission(false) != 0) {
    return false;
  }

  if (wire_.requestFrom(address, static_cast<uint8_t>(1)) != 1 ||
      !wire_.available()) {
    return false;
  }

  value = static_cast<uint8_t>(wire_.read());
  return true;
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
  const bool magnetometerStopped =
      sensor_.setMagDataRate(AK09916_MAG_DATARATE_SHUTDOWN);

  return accelFilterDisabled && gyroFilterDisabled && magnetometerStopped &&
         sensor_.getAccelRange() == ICM20948_ACCEL_RANGE_2_G &&
         sensor_.getGyroRange() == ICM20948_GYRO_RANGE_250_DPS &&
         sensor_.getAccelRateDivisor() == config::kIcmAccelRateDivisor &&
         sensor_.getGyroRateDivisor() == config::kIcmGyroRateDivisor;
}

}  // namespace pet::drivers
