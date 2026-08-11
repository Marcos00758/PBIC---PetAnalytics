#pragma once

#include <Arduino.h>
#include <SD.h>

#include "config/constants.h"
#include "data/imu_packet.h"
#include "drivers/bmp390.h"
#include "services/audio_capture.h"
#include "services/imu_acquisition.h"

namespace pet::services {

struct SdSessionMetadata {
  bool icmReady[data::kIcmCount] = {false, false, false};
  bool bmpReady[data::kBmpCount] = {false, false};
  bool bmpNvmValid[data::kBmpCount] = {false, false};
  uint8_t bmpAddress[data::kBmpCount] = {0, 0};
  uint8_t bmpNvm[data::kBmpCount][drivers::kBmp390NvmLength]{};
  bool audioEnabled = false;
  bool audioStartTimestampValid = false;
  uint32_t audioStartTimestampUs = 0;
};

struct SdLoggerCounters {
  uint32_t packetsQueued = 0;
  uint32_t packetsDropped = 0;
  uint32_t bytesWritten = 0;
  uint32_t writeAttempts = 0;
  uint32_t writeSuccesses = 0;
  uint32_t writeFailures = 0;
  uint32_t flushes = 0;
  uint32_t maxWriteDurationUs = 0;
  uint32_t maxFlushDurationUs = 0;
  uint32_t maxStatusDurationUs = 0;
  uint32_t slowWrites = 0;
  uint32_t slowFlushes = 0;
  uint32_t slowStatusUpdates = 0;
  uint32_t audioBlocksQueued = 0;
  uint32_t audioBlocksDropped = 0;
  uint32_t audioBytesWritten = 0;
  uint32_t audioWriteAttempts = 0;
  uint32_t audioWriteSuccesses = 0;
  uint32_t audioWriteFailures = 0;
  uint32_t audioFlushes = 0;
  uint32_t maxAudioWriteDurationUs = 0;
  uint32_t maxAudioFlushDurationUs = 0;
  uint32_t slowAudioWrites = 0;
  uint32_t slowAudioFlushes = 0;
};

class SdLogger {
 public:
  bool beginCard();
  bool beginSession(const SdSessionMetadata& metadata,
                    const AcquisitionCounters& acquisitionCounters,
                    const AudioCaptureCounters& audioCounters);
  bool enqueue(const data::ImuPacket& packet);
  bool enqueueAudio(const AudioPcmBlock& block);
  bool canEnqueueAudioBlock() const;
  void service(const AcquisitionCounters& acquisitionCounters,
               const AudioCaptureCounters& audioCounters);
  void updateFailureIndicator();

  bool cardReady() const { return cardReady_; }
  bool sessionActive() const { return sessionActive_; }
  bool failureConfirmed() const { return failureConfirmed_; }
  uint32_t sessionNumber() const { return sessionNumber_; }
  const char* sessionFolder() const { return sessionFolder_; }
  const SdLoggerCounters& counters() const { return counters_; }

 private:
  bool verifyReadWrite();
  bool chooseSessionNumber();
  bool persistSessionNumber();
  bool writeMetadata(const SdSessionMetadata& metadata);
  bool openAudioFile();
  bool writeStatus(const AcquisitionCounters& acquisitionCounters,
                   const AudioCaptureCounters& audioCounters,
                   const char* state);
  bool writeBufferedBytes(bool allowPartialBlock);
  bool writeAudioBufferedBytes(bool allowPartialBlock);
  void advanceBuffer(size_t count);
  void advanceAudioBuffer(size_t count);
  void checkWriteFailureTimeout();
  void confirmCardFailure(const char* reason);
  void recordOperationDuration(uint32_t durationUs, uint32_t& maximumUs,
                               uint32_t& slowOperations);
  void buildPath(char* destination, size_t length, const char* filename) const;

  File imuFile_;
  File audioFile_;
  uint8_t buffer_[config::kSdRamBufferBytes]{};
  uint8_t audioBuffer_[config::kSdAudioRamBufferBytes]{};
  size_t bufferHead_ = 0;
  size_t bufferTail_ = 0;
  size_t bufferedBytes_ = 0;
  size_t audioBufferHead_ = 0;
  size_t audioBufferTail_ = 0;
  size_t audioBufferedBytes_ = 0;
  uint32_t sessionNumber_ = 0;
  char sessionFolder_[16]{};
  uint32_t packetsAtLastFlush_ = 0;
  uint32_t packetsAtLastStatus_ = 0;
  uint32_t nextWriteRetryMs_ = 0;
  uint32_t imuWriteFailureStartedMs_ = 0;
  uint32_t audioWriteFailureStartedMs_ = 0;
  bool imuWriteFailureActive_ = false;
  bool audioWriteFailureActive_ = false;
  bool cardReady_ = false;
  bool sessionActive_ = false;
  bool failureConfirmed_ = false;
  bool failureIndicatorReady_ = false;
  uint32_t failureConfirmedAtMs_ = 0;
  SdSessionMetadata sessionMetadata_{};
  SdLoggerCounters counters_{};
};

}  // namespace pet::services
