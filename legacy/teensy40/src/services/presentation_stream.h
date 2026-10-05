#pragma once

#include <Arduino.h>

#include "data/imu_packet.h"
#include "drivers/bmp390.h"
#include "drivers/icm20948.h"
#include "drivers/pca9548a.h"
#include "services/imu_acquisition.h"

namespace pet::services {

// Bench mode that streams the unchanged v4 packet over USB for the live
// presentation tool. It never touches the SD card, so the demonstration runs
// without a memory card and cannot be interrupted by a card failure.
class PresentationStream {
 public:
  PresentationStream(drivers::Pca9548a& mux, drivers::Icm20948& icm0,
                     drivers::Icm20948& icm1, drivers::Icm20948& icm2,
                     drivers::Bmp390& bmp0, drivers::Bmp390& bmp1,
                     ImuAcquisition& acquisition);

  bool begin();
  void service();

  bool streaming() const { return streaming_; }

  static constexpr uint8_t kMuxChannelCount = 8;
  static constexpr uint8_t kMaxScanAddresses = 8;

 private:
  bool initializeSensors();
  void scanBus();
  void resetBus() const;
  void printBanner();
  void printHeaderLine() const;
  void printScanLine(uint8_t channel) const;
  void printIcmLine(size_t index) const;
  void printBmpLine(size_t index) const;
  void printBmpNvmLine(size_t index) const;

  drivers::Pca9548a& mux_;
  drivers::Icm20948* icms_[data::kIcmCount];
  drivers::Bmp390* bmps_[data::kBmpCount];
  ImuAcquisition& acquisition_;
  uint8_t bmpNvm_[data::kBmpCount][drivers::kBmp390NvmLength]{};
  bool bmpNvmValid_[data::kBmpCount] = {false, false};
  bool icmReady_[data::kIcmCount] = {false, false, false};
  bool bmpReady_[data::kBmpCount] = {false, false};
  bool bmpBeginOk_[data::kBmpCount] = {false, false};
  bool bmpSamplingOk_[data::kBmpCount] = {false, false};
  uint8_t scanAddresses_[kMuxChannelCount][kMaxScanAddresses]{};
  uint8_t scanCount_[kMuxChannelCount] = {0, 0, 0, 0, 0, 0, 0, 0};
  const char* errorReason_ = nullptr;
  bool scanned_ = false;
  uint32_t nextBannerMs_ = 0;
  bool streaming_ = false;
};

}  // namespace pet::services
