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

void appendMagnetometerValues(const drivers::Vector3i16& sample,
                              int16_t* values) {
  values[6] = sample.x;
  values[7] = sample.y;
  values[8] = sample.z;
}

bool vectorsEqual(const drivers::Vector3i16& left,
                  const drivers::Vector3i16& right) {
  return left.x == right.x && left.y == right.y && left.z == right.z;
}

}  // namespace

ImuAcquisition::ImuAcquisition(drivers::Icm20948& icm0,
                               drivers::Icm20948& icm1,
                               drivers::Icm20948& icm2,
                               drivers::Bmp390& bmp0,
                               drivers::Bmp390& bmp1,
                               uint32_t samplePeriodUs,
                               uint32_t bmpSamplesPerImuSample,
                               uint32_t magSamplesPerImuSample)
    : icms_{&icm0, &icm1, &icm2},
      bmps_{&bmp0, &bmp1},
      samplePeriodUs_(samplePeriodUs),
      bmpSamplesPerImuSample_(bmpSamplesPerImuSample),
      magSamplesPerImuSample_(magSamplesPerImuSample) {}

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
    appendSampleValues(sample, &values[i * data::kValuesPerIcm]);
  }

  if (!roundSucceeded) {
    ++counters_.failedAcquisitionRounds;
    return false;
  }

  if (bmpSamplesPerImuSample_ > 0 &&
      bmpSamplesPerImuSample_ >= data::kBmpCount) {
    for (size_t i = 0; i < data::kBmpCount; ++i) {
      const uint32_t phase =
          (i == 0) ? bmpSamplesPerImuSample_ - 1U : 0U;
      const bool cacheNeedsPriming =
          bmpRaw_[i].pressure == data::kInvalidBmpRaw;
      if (!cacheNeedsPriming &&
          packetSequence % bmpSamplesPerImuSample_ != phase) {
        continue;
      }

      drivers::Bmp390RawSample sample{};
      if (!bmps_[i]->readRaw(sample)) {
        ++counters_.bmpI2cFailures[i];
        continue;
      }
      bmpRaw_[i] = {sample.pressure, sample.temperature};
      ++counters_.bmpUpdates[i];
    }
  }

  if (magSamplesPerImuSample_ > 0) {
    for (size_t i = 0; i < data::kIcmCount; ++i) {
      const bool cacheNeedsPriming = !magCacheValid_[i];
      const bool sensorIsDue =
          packetSequence % magSamplesPerImuSample_ == i;
      if (!cacheNeedsPriming && !sensorIsDue) {
        continue;
      }

      drivers::Ak09916RawSample sample{};
      if (!icms_[i]->readMagnetometerRaw(sample)) {
        ++counters_.magI2cFailures[i];
        continue;
      }
      if (sample.dataOverrun) {
        ++counters_.magDataOverruns[i];
      }
      if (sample.overflow) {
        ++counters_.magOverflows[i];
        continue;
      }

      const bool valuesChanged =
          !magCacheValid_[i] || !vectorsEqual(magRaw_[i], sample.magnetic);
      if (!sample.dataReady && !valuesChanged) {
        ++counters_.magNoNewData[i];
        continue;
      }

      magRaw_[i] = sample.magnetic;
      magUpdatedAtUs_[i] = nowUs;
      magCacheValid_[i] = true;
      ++counters_.magUpdates[i];
    }
  }

  for (size_t i = 0; i < data::kIcmCount; ++i) {
    appendMagnetometerValues(magRaw_[i],
                             &values[i * data::kValuesPerIcm]);
  }

  utils::buildImuPacket(packet, nowUs, packetSequence, values, bmpRaw_);
  ++counters_.packetsProduced;
  return true;
}

void ImuAcquisition::recordUsbDrop() {
  ++counters_.usbDroppedPackets;
}

}  // namespace pet::services
