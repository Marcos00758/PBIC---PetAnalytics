#include "services/imu_acquisition.h"

#include "utils/packet.h"

namespace pet::services {
namespace {

void appendSampleValues(const drivers::Icm20948RawSample& sample,
                        int16_t* values) {
  values[0] = sample.acceleration.x;
  values[1] = sample.acceleration.y;
  values[2] = sample.acceleration.z;
  values[3] = sample.gyro.x;
  values[4] = sample.gyro.y;
  values[5] = sample.gyro.z;
}

}  // namespace

ImuAcquisition::ImuAcquisition(drivers::Icm20948& icm0,
                               drivers::Icm20948& icm1,
                               drivers::Icm20948& icm2,
                               uint32_t samplePeriodUs)
    : icms_{&icm0, &icm1, &icm2}, samplePeriodUs_(samplePeriodUs) {}

void ImuAcquisition::start(uint32_t nowUs) {
  nextSampleUs_ = nowUs + samplePeriodUs_;
  started_ = true;
}

bool ImuAcquisition::poll(data::ImuPacket& packet) {
  if (!started_ || samplePeriodUs_ == 0) {
    return false;
  }

  const uint32_t nowUs = micros();
  if (static_cast<int32_t>(nowUs - nextSampleUs_) < 0) {
    return false;
  }

  const uint32_t latePeriods = (nowUs - nextSampleUs_) / samplePeriodUs_;
  counters_.missedScheduleReadings += latePeriods;
  sequence_ = static_cast<uint16_t>(sequence_ + latePeriods);
  nextSampleUs_ += (latePeriods + 1U) * samplePeriodUs_;

  const uint16_t packetSequence = sequence_++;
  int16_t values[data::kImuValueCount]{};
  bool roundSucceeded = true;

  for (size_t i = 0; i < data::kIcmCount; ++i) {
    drivers::Icm20948RawSample sample{};
    if (!icms_[i]->readRaw(sample)) {
      ++counters_.i2cFailures[i];
      roundSucceeded = false;
      continue;
    }
    appendSampleValues(sample, &values[i * data::kAxesPerIcm]);
  }

  if (!roundSucceeded) {
    ++counters_.failedAcquisitionRounds;
    return false;
  }

  utils::buildImuPacket(packet, nowUs, packetSequence, values);
  ++counters_.packetsProduced;
  return true;
}

void ImuAcquisition::recordUsbDrop() {
  ++counters_.usbDroppedPackets;
}

}  // namespace pet::services
