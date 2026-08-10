#pragma once

#include <Arduino.h>

#include "data/imu_packet.h"
#include "drivers/icm20948.h"
#include "drivers/bmp390.h"

namespace pet::services {

struct AcquisitionCounters {
  uint32_t packetsProduced = 0;
  uint32_t missedScheduleReadings = 0;
  uint32_t failedAcquisitionRounds = 0;
  uint32_t i2cFailures[data::kIcmCount] = {0, 0, 0};
  uint32_t bmpI2cFailures[data::kBmpCount] = {0, 0};
  uint32_t bmpUpdates[data::kBmpCount] = {0, 0};
  uint32_t magI2cFailures[data::kIcmCount] = {0, 0, 0};
  uint32_t magUpdates[data::kIcmCount] = {0, 0, 0};
  uint32_t magNoNewData[data::kIcmCount] = {0, 0, 0};
  uint32_t magDataOverruns[data::kIcmCount] = {0, 0, 0};
  uint32_t magOverflows[data::kIcmCount] = {0, 0, 0};
  uint32_t usbDroppedPackets = 0;
};

class ImuAcquisition {
 public:
  ImuAcquisition(drivers::Icm20948& icm0, drivers::Icm20948& icm1,
                 drivers::Icm20948& icm2, drivers::Bmp390& bmp0,
                 drivers::Bmp390& bmp1, uint32_t samplePeriodUs,
                 uint32_t bmpSamplesPerImuSample,
                 uint32_t magSamplesPerImuSample);

  void start(uint32_t nowUs);
  bool poll(data::ImuPacket& packet);
  void recordUsbDrop();

  const AcquisitionCounters& counters() const { return counters_; }

 private:
  drivers::Icm20948* icms_[data::kIcmCount];
  drivers::Bmp390* bmps_[data::kBmpCount];
  const uint32_t samplePeriodUs_;
  const uint32_t bmpSamplesPerImuSample_;
  const uint32_t magSamplesPerImuSample_;
  data::BmpRawValues bmpRaw_[data::kBmpCount] = {
      {data::kInvalidBmpRaw, data::kInvalidBmpRaw},
      {data::kInvalidBmpRaw, data::kInvalidBmpRaw}};
  drivers::Vector3i16 magRaw_[data::kIcmCount]{};
  uint32_t magUpdatedAtUs_[data::kIcmCount] = {0, 0, 0};
  bool magCacheValid_[data::kIcmCount] = {false, false, false};
  uint32_t nextSampleUs_ = 0;
  uint16_t sequence_ = 0;
  bool started_ = false;
  AcquisitionCounters counters_{};
};

}  // namespace pet::services
