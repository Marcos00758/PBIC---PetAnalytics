#pragma once

#include <Arduino.h>

#include "data/imu_packet.h"
#include "drivers/icm20948.h"
#include "drivers/bmp390.h"

namespace pet::services {

constexpr size_t kBmpCount = 2;

struct AcquisitionCounters {
  uint32_t packetsProduced = 0;
  uint32_t missedScheduleReadings = 0;
  uint32_t failedAcquisitionRounds = 0;
  uint32_t i2cFailures[data::kIcmCount] = {0, 0, 0};
  uint32_t bmpI2cFailures[kBmpCount] = {0, 0};
  uint32_t bmpUpdates[kBmpCount] = {0, 0};
  uint32_t usbDroppedPackets = 0;
};

class ImuAcquisition {
 public:
  ImuAcquisition(drivers::Icm20948& icm0, drivers::Icm20948& icm1,
                 drivers::Icm20948& icm2, drivers::Bmp390& bmp0,
                 drivers::Bmp390& bmp1, uint32_t samplePeriodUs,
                 uint32_t bmpSamplesPerImuSample);

  void start(uint32_t nowUs);
  bool poll(data::ImuPacket& packet);
  void recordUsbDrop();

  const AcquisitionCounters& counters() const { return counters_; }

 private:
  drivers::Icm20948* icms_[data::kIcmCount];
  drivers::Bmp390* bmps_[kBmpCount];
  const uint32_t samplePeriodUs_;
  const uint32_t bmpSamplesPerImuSample_;
  uint32_t bmpPressureRaw_[kBmpCount] = {0, 0};
  uint32_t bmpTemperatureRaw_[kBmpCount] = {0, 0};
  uint32_t nextSampleUs_ = 0;
  uint16_t sequence_ = 0;
  bool started_ = false;
  AcquisitionCounters counters_{};
};

}  // namespace pet::services
