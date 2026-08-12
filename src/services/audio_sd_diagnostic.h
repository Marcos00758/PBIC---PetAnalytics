#pragma once

#include <Arduino.h>
#include <SD.h>

#include "config/constants.h"
#include "services/audio_capture.h"

namespace pet::services {

constexpr size_t kAudioSdLatencyBucketCount = 8;

struct AudioSdPhaseMetrics {
  uint32_t bytesWritten = 0;
  uint32_t writeAttempts = 0;
  uint32_t writeFailures = 0;
  uint32_t silenceBlocksInserted = 0;
  uint32_t gapEvents = 0;
  uint32_t maxGapBlocks = 0;
  uint32_t maxBufferedBytes = 0;
  uint32_t maxWriteDurationUs = 0;
  uint32_t writeLatencyBuckets[kAudioSdLatencyBucketCount]{};
};

struct AudioSdDiagnosticCounters {
  uint32_t blocksQueued = 0;
  uint32_t silenceBlocksInserted = 0;
  uint32_t gapEvents = 0;
  uint32_t maxGapBlocks = 0;
  uint32_t bytesWritten = 0;
  uint32_t durableBytes = 0;
  uint32_t writeAttempts = 0;
  uint32_t writeFailures = 0;
  uint32_t partialWrites = 0;
  uint32_t flushes = 0;
  uint32_t journalUpdates = 0;
  uint32_t maxBufferedBytes = 0;
  uint32_t maxWriteDurationUs = 0;
  uint32_t maxFlushDurationUs = 0;
  uint32_t maxJournalDurationUs = 0;
  uint32_t slowWrites = 0;
  uint32_t slowFlushes = 0;
  uint32_t slowJournalUpdates = 0;
  AudioSdPhaseMetrics phases[config::kAudioSdDiagnosticPhaseCount]{};
};

class AudioSdDiagnostic {
 public:
  bool begin();
  bool start(const AudioPreflightResult& preflight,
             uint32_t firstSampleTimestampUs,
             const AudioCaptureCounters& captureCounters);
  bool canEnqueueAudioBlock() const;
  bool enqueueAudio(const AudioPcmBlock& block);
  void service(const AudioCaptureCounters& captureCounters);

  bool recording() const { return state_ == State::kRecording; }
  bool finished() const { return state_ == State::kFinished; }
  bool failed() const { return state_ == State::kFailed; }
  const char* folder() const { return folder_; }
  const AudioSdDiagnosticCounters& counters() const { return counters_; }

 private:
  enum class State : uint8_t {
    kIdle,
    kReady,
    kRecording,
    kDraining,
    kFinalFlush,
    kTruncate,
    kFinalJournal,
    kFinalStatus,
    kClose,
    kFinished,
    kFailed,
  };

  bool verifyReadWrite();
  bool chooseFolder();
  bool openAndPreallocate();
  bool writeMetadata(const AudioPreflightResult& preflight);
  bool writeJournal(const char* state);
  bool writeStatus(const AudioCaptureCounters& captureCounters,
                   const char* state);
  bool appendSamples(const int16_t* samples);
  bool writeBufferedBytes(bool allowPartialBlock);
  bool flushAudio();
  void fail(const char* reason);
  void buildPath(char* destination, size_t length,
                 const char* filename) const;
  void recordDuration(uint32_t durationUs, uint32_t& maximumUs,
                      uint32_t& slowOperations);
  void recordWriteLatency(uint32_t durationUs);
  void updatePhase(uint32_t elapsedMs);
  size_t writeBlockBytes() const;

  FsFile audioFile_;
  uint8_t buffer_[config::kSdAudioRamBufferBytes]{};
  uint8_t writeScratch_[config::kAudioSdDiagnosticBlockBytes[1]]{};
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t bufferedBytes_ = 0;
  uint64_t preallocatedBytes_ = 0;
  uint32_t startedMs_ = 0;
  uint32_t firstSampleTimestampUs_ = 0;
  uint32_t nextSequence_ = 0;
  uint32_t nextFlushBytes_ = 0;
  uint32_t nextJournalBytes_ = 0;
  size_t phaseIndex_ = 0;
  bool sequenceInitialized_ = false;
  bool gapInProgress_ = false;
  bool flushPending_ = false;
  bool journalPending_ = false;
  char folder_[16]{};
  AudioCaptureCounters captureAtStart_{};
  AudioSdDiagnosticCounters counters_{};
  State state_ = State::kIdle;
};

}  // namespace pet::services
