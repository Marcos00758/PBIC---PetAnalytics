#pragma once

#include <Arduino.h>
#include <SD.h>

#include "config/constants.h"
#include "data/imu_packet.h"
#include "drivers/bmp390.h"
#include "services/imu_acquisition.h"

namespace pet::services {

struct SdSessionMetadata {
  bool icmReady[data::kIcmCount] = {false, false, false};
  bool bmpReady[data::kBmpCount] = {false, false};
  bool bmpNvmValid[data::kBmpCount] = {false, false};
  uint8_t bmpAddress[data::kBmpCount] = {0, 0};
  uint8_t bmpNvm[data::kBmpCount][drivers::kBmp390NvmLength]{};
};

struct SdLoggerCounters {
  uint32_t packetsQueued = 0;
  uint32_t packetsDropped = 0;
  uint32_t bytesWritten = 0;
  uint32_t writeAttempts = 0;
  uint32_t writeSuccesses = 0;
  uint32_t writeFailures = 0;
  uint32_t flushes = 0;
};

class SdLogger {
 public:
  bool beginCard();
  bool beginSession(const SdSessionMetadata& metadata,
                    const AcquisitionCounters& acquisitionCounters);
  bool enqueue(const data::ImuPacket& packet);
  void service(const AcquisitionCounters& acquisitionCounters);

  bool cardReady() const { return cardReady_; }
  bool sessionActive() const { return sessionActive_; }
  uint32_t sessionNumber() const { return sessionNumber_; }
  const char* sessionFolder() const { return sessionFolder_; }
  const SdLoggerCounters& counters() const { return counters_; }

 private:
  bool verifyReadWrite();
  bool chooseSessionNumber();
  bool persistSessionNumber();
  bool writeMetadata(const SdSessionMetadata& metadata);
  bool createAudioPlaceholder();
  bool writeStatus(const AcquisitionCounters& acquisitionCounters,
                   const char* state);
  bool writeBufferedBytes(bool allowPartialBlock);
  void advanceBuffer(size_t count);
  void checkHealthWindow();
  void handleCardFailure();
  void buildPath(char* destination, size_t length, const char* filename) const;

  File imuFile_;
  uint8_t buffer_[config::kSdRamBufferBytes]{};
  size_t bufferHead_ = 0;
  size_t bufferTail_ = 0;
  size_t bufferedBytes_ = 0;
  uint32_t sessionNumber_ = 0;
  char sessionFolder_[16]{};
  uint32_t packetsAtLastFlush_ = 0;
  uint32_t packetsAtLastStatus_ = 0;
  uint32_t nextWriteRetryMs_ = 0;
  uint32_t healthWindowStartedMs_ = 0;
  bool writeAttemptedInWindow_ = false;
  bool writeSucceededInWindow_ = false;
  bool cardReady_ = false;
  bool sessionActive_ = false;
  SdSessionMetadata sessionMetadata_{};
  SdLoggerCounters counters_{};
};

}  // namespace pet::services
