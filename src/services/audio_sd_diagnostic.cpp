#include "services/audio_sd_diagnostic.h"

#include <SPI.h>
#include <string.h>

#include "config/pins.h"

namespace pet::services {
namespace {

constexpr char kTestPath[] = "/pbic_mic_sd_test.tmp";
constexpr uint8_t kTestPattern[] = {0x50, 0x42, 0x49, 0x43, 0x4D, 0x49, 0x43};

constexpr uint32_t audioBytesPerSecond() {
  return config::kMicrophoneSampleRateHz * config::kMicrophoneChannels *
         (config::kMicrophoneBitsPerSample / 8U);
}

constexpr uint64_t diagnosticPreallocationBytes() {
  return static_cast<uint64_t>(audioBytesPerSecond()) *
         (config::kAudioSdDiagnosticDurationSeconds +
          config::kSdPreallocationMarginSeconds);
}

}  // namespace

bool AudioSdDiagnostic::begin() {
  counters_ = {};
  state_ = State::kIdle;
  pinMode(pins::kSdChipSelect, OUTPUT);
  digitalWrite(pins::kSdChipSelect, HIGH);
  const SdSpiConfig spiConfig(pins::kSdChipSelect, SHARED_SPI,
                              SD_SCK_MHZ(config::kSdSpiClockMHz));
  if (!SD.sdfs.begin(spiConfig)) {
    fail("initialization");
    return false;
  }
  if (!verifyReadWrite()) {
    fail("read_write_diagnostic");
    return false;
  }
  if (!chooseFolder() || !SD.mkdir(folder_)) {
    fail("session_directory");
    return false;
  }
  if (!openAndPreallocate()) {
    fail("audio_preallocation");
    return false;
  }
  state_ = State::kReady;
  return true;
}

bool AudioSdDiagnostic::start(
    const AudioPreflightResult& preflight, uint32_t firstSampleTimestampUs,
    const AudioCaptureCounters& captureCounters) {
  if (state_ != State::kReady) {
    return false;
  }
  captureAtStart_ = captureCounters;
  firstSampleTimestampUs_ = firstSampleTimestampUs;
  head_ = 0;
  tail_ = 0;
  bufferedBytes_ = 0;
  sequenceInitialized_ = false;
  gapInProgress_ = false;
  flushPending_ = false;
  journalPending_ = false;
  nextFlushBytes_ = audioBytesPerSecond() *
                    config::kAudioSdDiagnosticFlushSeconds;
  nextJournalBytes_ = audioBytesPerSecond() *
                      config::kAudioSdDiagnosticJournalSeconds;
  startedMs_ = millis();
  if (!writeMetadata(preflight) || !writeJournal("recording") ||
      !writeStatus(captureCounters, "recording")) {
    fail("initial_session_files");
    return false;
  }
  state_ = State::kRecording;
  Serial.print("MIC_SD_TEST_START folder=");
  Serial.print(folder_);
  Serial.print(" duration_s=");
  Serial.print(config::kAudioSdDiagnosticDurationSeconds);
  Serial.print(" preallocated_bytes=");
  Serial.print(static_cast<uint32_t>(preallocatedBytes_));
  Serial.print(" block_bytes=");
  Serial.println(config::kSdAudioWriteBlockBytes);
  return true;
}

bool AudioSdDiagnostic::canEnqueueAudioBlock() const {
  return state_ == State::kRecording &&
         kAudioPcmBytesPerBlock <=
             config::kSdAudioRamBufferBytes - bufferedBytes_;
}

bool AudioSdDiagnostic::enqueueAudio(const AudioPcmBlock& block) {
  if (state_ != State::kRecording) {
    return false;
  }
  if (!sequenceInitialized_) {
    nextSequence_ = block.sequence;
    sequenceInitialized_ = true;
  }

  const uint32_t missingBlocks = block.sequence - nextSequence_;
  if (missingBlocks != 0 && missingBlocks < 0x80000000UL) {
    if (!gapInProgress_) {
      gapInProgress_ = true;
      ++counters_.gapEvents;
      if (missingBlocks > counters_.maxGapBlocks) {
        counters_.maxGapBlocks = missingBlocks;
      }
    }
    static const int16_t silence[config::kMicrophoneBlockSamples]{};
    while (nextSequence_ != block.sequence && canEnqueueAudioBlock()) {
      if (!appendSamples(silence)) {
        return false;
      }
      ++nextSequence_;
      ++counters_.silenceBlocksInserted;
    }
    if (nextSequence_ != block.sequence) {
      return false;
    }
  } else if (missingBlocks >= 0x80000000UL) {
    return true;
  }

  if (!canEnqueueAudioBlock() || !appendSamples(block.samples)) {
    return false;
  }
  nextSequence_ = block.sequence + 1U;
  gapInProgress_ = false;
  ++counters_.blocksQueued;
  return true;
}

void AudioSdDiagnostic::service(
    const AudioCaptureCounters& captureCounters) {
  if (state_ == State::kRecording &&
      millis() - startedMs_ >=
          config::kAudioSdDiagnosticDurationSeconds * 1000UL) {
    state_ = State::kDraining;
    Serial.print("MIC_SD_TEST_STOP_REQUESTED buffered_bytes=");
    Serial.println(bufferedBytes_);
  }

  if (state_ == State::kRecording) {
    if (!flushPending_ && counters_.bytesWritten >= nextFlushBytes_) {
      flushPending_ = true;
    }
    if (!journalPending_ && counters_.bytesWritten >= nextJournalBytes_) {
      journalPending_ = true;
    }

    const bool bufferBelowUrgent =
        bufferedBytes_ * 2U < config::kSdAudioRamBufferBytes;
    if (flushPending_ && bufferBelowUrgent) {
      if (!flushAudio()) {
        fail("periodic_flush");
        return;
      }
      flushPending_ = false;
      nextFlushBytes_ += audioBytesPerSecond() *
                         config::kAudioSdDiagnosticFlushSeconds;
      return;
    }
    if (journalPending_ && !flushPending_ && bufferBelowUrgent) {
      if (!writeJournal("recording")) {
        fail("journal_update");
        return;
      }
      journalPending_ = false;
      nextJournalBytes_ += audioBytesPerSecond() *
                           config::kAudioSdDiagnosticJournalSeconds;
      return;
    }
    if (bufferedBytes_ >= config::kSdAudioWriteBlockBytes &&
        !writeBufferedBytes(false)) {
      fail("audio_write");
    }
    return;
  }

  if (state_ == State::kDraining) {
    if (bufferedBytes_ >= config::kSdAudioWriteBlockBytes) {
      if (!writeBufferedBytes(false)) {
        fail("final_audio_write");
      }
      return;
    }
    if (bufferedBytes_ > 0) {
      if (!writeBufferedBytes(true)) {
        fail("final_partial_write");
      }
      return;
    }
    state_ = State::kFinalFlush;
  }

  if (state_ == State::kFinalFlush) {
    if (!flushAudio()) {
      fail("final_flush");
      return;
    }
    state_ = State::kTruncate;
    return;
  }
  if (state_ == State::kTruncate) {
    if (!audioFile_.truncate(counters_.bytesWritten)) {
      fail("truncate");
      return;
    }
    state_ = State::kFinalJournal;
    return;
  }
  if (state_ == State::kFinalJournal) {
    if (!writeJournal("completed_duration")) {
      fail("final_journal");
      return;
    }
    state_ = State::kFinalStatus;
    return;
  }
  if (state_ == State::kFinalStatus) {
    if (!writeStatus(captureCounters, "completed_duration")) {
      fail("final_status");
      return;
    }
    state_ = State::kClose;
    return;
  }
  if (state_ == State::kClose) {
    audioFile_.close();
    state_ = State::kFinished;
    Serial.print("MIC_SD_TEST_COMPLETED folder=");
    Serial.print(folder_);
    Serial.print(" audio_bytes=");
    Serial.print(counters_.bytesWritten);
    Serial.print(" duration_s=");
    Serial.print(static_cast<double>(counters_.bytesWritten) /
                     audioBytesPerSecond(),
                 3);
    Serial.print(" capture_drops=");
    Serial.print(captureCounters.blocksDropped -
                 captureAtStart_.blocksDropped);
    Serial.print(" silence_blocks=");
    Serial.println(counters_.silenceBlocksInserted);
  }
}

bool AudioSdDiagnostic::verifyReadWrite() {
  SD.remove(kTestPath);
  File output = SD.open(kTestPath, FILE_WRITE);
  if (!output) {
    return false;
  }
  const size_t written = output.write(kTestPattern, sizeof(kTestPattern));
  output.flush();
  output.close();
  if (written != sizeof(kTestPattern)) {
    SD.remove(kTestPath);
    return false;
  }

  uint8_t received[sizeof(kTestPattern)]{};
  File input = SD.open(kTestPath, FILE_READ);
  if (!input) {
    SD.remove(kTestPath);
    return false;
  }
  const int count = input.read(received, sizeof(received));
  input.close();
  SD.remove(kTestPath);
  return count == static_cast<int>(sizeof(received)) &&
         memcmp(received, kTestPattern, sizeof(received)) == 0;
}

bool AudioSdDiagnostic::chooseFolder() {
  for (uint32_t number = 1; number < 1000; ++number) {
    snprintf(folder_, sizeof(folder_), "/M%03lu",
             static_cast<unsigned long>(number));
    if (!SD.exists(folder_)) {
      return true;
    }
  }
  return false;
}

bool AudioSdDiagnostic::openAndPreallocate() {
  char path[32]{};
  buildPath(path, sizeof(path), "audio.raw");
  preallocatedBytes_ = diagnosticPreallocationBytes();
  const uint32_t startedUs = micros();
  audioFile_ = SD.sdfs.open(path, O_RDWR | O_CREAT | O_TRUNC);
  if (!audioFile_ || !audioFile_.preAllocate(preallocatedBytes_) ||
      !audioFile_.seekSet(0)) {
    if (audioFile_) {
      audioFile_.truncate(0);
      audioFile_.close();
    }
    return false;
  }
  Serial.print("MIC_SD_TEST_PREALLOCATE folder=");
  Serial.print(folder_);
  Serial.print(" audio_bytes=");
  Serial.print(static_cast<uint32_t>(preallocatedBytes_));
  Serial.print(" duration_us=");
  Serial.println(micros() - startedUs);
  return true;
}

bool AudioSdDiagnostic::writeMetadata(
    const AudioPreflightResult& preflight) {
  char path[32]{};
  buildPath(path, sizeof(path), "meta.txt");
  SD.remove(path);
  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    return false;
  }
  file.println("mode=audio_sd_diagnostic");
  file.print("firmware_version=");
  file.println(config::kFirmwareVersion);
  file.println("board=Teensy 4.0");
  file.println("audio_file=audio.raw");
  file.println("audio_format=pcm_s16le");
  file.print("audio_sample_rate_hz=");
  file.println(config::kMicrophoneSampleRateHz);
  file.println("audio_channels=1");
  file.println("audio_bits_per_sample=16");
  file.print("audio_block_samples=");
  file.println(config::kMicrophoneBlockSamples);
  file.print("audio_start_timestamp_us=");
  file.println(firstSampleTimestampUs_);
  file.print("duration_seconds=");
  file.println(config::kAudioSdDiagnosticDurationSeconds);
  file.print("preallocated_bytes=");
  file.println(static_cast<uint32_t>(preallocatedBytes_));
  file.print("sd_spi_clock_mhz=");
  file.println(config::kSdSpiClockMHz);
  file.print("sd_write_block_bytes=");
  file.println(config::kSdAudioWriteBlockBytes);
  file.print("audio_capture_queue_usable_blocks=");
  file.println(config::kMicrophoneQueueBlocks - 1U);
  file.println("audio_gap_policy=zero_fill");
  file.print("audio_preflight_valid=");
  file.println(preflight.valid ? 1 : 0);
  file.print("audio_preflight_accepted=");
  file.println(preflight.accepted ? 1 : 0);
  file.print("audio_preflight_mean_counts=");
  file.println(preflight.meanCounts);
  file.print("audio_preflight_rms_counts=");
  file.println(preflight.rmsCounts);
  file.print("audio_preflight_peak_counts=");
  file.println(preflight.peakCounts);
  file.print("audio_preflight_clipping_samples=");
  file.println(preflight.clippingSamples);
  file.flush();
  file.close();
  return true;
}

bool AudioSdDiagnostic::writeJournal(const char* state) {
  char path[32]{};
  char temporaryPath[32]{};
  buildPath(path, sizeof(path), "journal.txt");
  buildPath(temporaryPath, sizeof(temporaryPath), "journal.tmp");
  SD.remove(temporaryPath);
  const uint32_t startedUs = micros();
  File file = SD.open(temporaryPath, FILE_WRITE);
  if (!file) {
    return false;
  }
  file.print("state=");
  file.println(state);
  file.print("audio_valid_bytes=");
  file.println(counters_.durableBytes);
  file.print("audio_silence_blocks_inserted=");
  file.println(counters_.silenceBlocksInserted);
  file.print("audio_gap_events=");
  file.println(counters_.gapEvents);
  file.print("audio_max_gap_blocks=");
  file.println(counters_.maxGapBlocks);
  file.print("audio_start_timestamp_valid=");
  file.println(firstSampleTimestampUs_ != 0 ? 1 : 0);
  file.print("audio_start_timestamp_us=");
  file.println(firstSampleTimestampUs_);
  file.print("audio_preallocated_bytes=");
  file.println(static_cast<uint32_t>(preallocatedBytes_));
  file.flush();
  file.close();
  SD.remove(path);
  if (!SD.rename(temporaryPath, path)) {
    return false;
  }
  recordDuration(micros() - startedUs, counters_.maxJournalDurationUs,
                 counters_.slowJournalUpdates);
  ++counters_.journalUpdates;
  return true;
}

bool AudioSdDiagnostic::writeStatus(
    const AudioCaptureCounters& captureCounters, const char* state) {
  char path[32]{};
  char temporaryPath[32]{};
  buildPath(path, sizeof(path), "status.txt");
  buildPath(temporaryPath, sizeof(temporaryPath), "status.tmp");
  SD.remove(temporaryPath);
  File file = SD.open(temporaryPath, FILE_WRITE);
  if (!file) {
    return false;
  }
  file.print("state=");
  file.println(state);
  file.print("elapsed_ms=");
  file.println(millis() - startedMs_);
  file.print("audio_blocks_received=");
  file.println(captureCounters.blocksReceived - captureAtStart_.blocksReceived);
  file.print("audio_capture_blocks_dropped=");
  file.println(captureCounters.blocksDropped - captureAtStart_.blocksDropped);
  file.print("audio_incomplete_blocks=");
  file.println(captureCounters.incompleteBlocks -
               captureAtStart_.incompleteBlocks);
  file.print("audio_capture_queue_high_water_blocks=");
  file.println(captureCounters.queueHighWaterBlocks);
  file.print("sd_audio_blocks_queued=");
  file.println(counters_.blocksQueued);
  file.print("sd_audio_silence_blocks_inserted=");
  file.println(counters_.silenceBlocksInserted);
  file.print("sd_audio_gap_events=");
  file.println(counters_.gapEvents);
  file.print("sd_audio_max_gap_blocks=");
  file.println(counters_.maxGapBlocks);
  file.print("sd_audio_bytes_written=");
  file.println(counters_.bytesWritten);
  file.print("sd_audio_write_attempts=");
  file.println(counters_.writeAttempts);
  file.print("sd_audio_write_failures=");
  file.println(counters_.writeFailures);
  file.print("sd_audio_partial_writes=");
  file.println(counters_.partialWrites);
  file.print("sd_audio_flushes=");
  file.println(counters_.flushes);
  file.print("sd_audio_max_buffered_bytes=");
  file.println(counters_.maxBufferedBytes);
  file.print("sd_audio_max_write_duration_us=");
  file.println(counters_.maxWriteDurationUs);
  file.print("sd_audio_max_flush_duration_us=");
  file.println(counters_.maxFlushDurationUs);
  file.print("sd_audio_slow_writes_over_10ms=");
  file.println(counters_.slowWrites);
  file.print("sd_audio_slow_flushes_over_10ms=");
  file.println(counters_.slowFlushes);
  file.print("sd_journal_updates=");
  file.println(counters_.journalUpdates);
  file.print("sd_max_journal_duration_us=");
  file.println(counters_.maxJournalDurationUs);
  file.print("sd_slow_journal_updates_over_10ms=");
  file.println(counters_.slowJournalUpdates);
  file.flush();
  file.close();
  SD.remove(path);
  return SD.rename(temporaryPath, path);
}

bool AudioSdDiagnostic::appendSamples(const int16_t* samples) {
  if (kAudioPcmBytesPerBlock >
      config::kSdAudioRamBufferBytes - bufferedBytes_) {
    return false;
  }
  for (size_t index = 0; index < config::kMicrophoneBlockSamples; ++index) {
    const uint16_t sample = static_cast<uint16_t>(samples[index]);
    buffer_[tail_] = static_cast<uint8_t>(sample & 0xFFU);
    tail_ = (tail_ + 1U) % config::kSdAudioRamBufferBytes;
    buffer_[tail_] = static_cast<uint8_t>(sample >> 8U);
    tail_ = (tail_ + 1U) % config::kSdAudioRamBufferBytes;
  }
  bufferedBytes_ += kAudioPcmBytesPerBlock;
  if (bufferedBytes_ > counters_.maxBufferedBytes) {
    counters_.maxBufferedBytes = bufferedBytes_;
  }
  return true;
}

bool AudioSdDiagnostic::writeBufferedBytes(bool allowPartialBlock) {
  if (bufferedBytes_ == 0 ||
      (!allowPartialBlock &&
       bufferedBytes_ < config::kSdAudioWriteBlockBytes)) {
    return false;
  }
  size_t count = bufferedBytes_;
  if (count > config::kSdAudioWriteBlockBytes) {
    count = config::kSdAudioWriteBlockBytes;
  }
  const size_t contiguous = config::kSdAudioRamBufferBytes - head_;
  const uint8_t* source = &buffer_[head_];
  if (count > contiguous) {
    memcpy(writeScratch_, source, contiguous);
    memcpy(writeScratch_ + contiguous, buffer_, count - contiguous);
    source = writeScratch_;
  }
  if (count < config::kSdAudioWriteBlockBytes) {
    ++counters_.partialWrites;
  }
  ++counters_.writeAttempts;
  const uint32_t startedUs = micros();
  const size_t written = audioFile_.write(source, count);
  recordDuration(micros() - startedUs, counters_.maxWriteDurationUs,
                 counters_.slowWrites);
  if (written != count) {
    ++counters_.writeFailures;
    return false;
  }
  head_ = (head_ + written) % config::kSdAudioRamBufferBytes;
  bufferedBytes_ -= written;
  counters_.bytesWritten += written;
  return true;
}

bool AudioSdDiagnostic::flushAudio() {
  const uint32_t startedUs = micros();
  const bool success = audioFile_.sync();
  recordDuration(micros() - startedUs, counters_.maxFlushDurationUs,
                 counters_.slowFlushes);
  ++counters_.flushes;
  if (success) {
    counters_.durableBytes = counters_.bytesWritten;
  }
  return success;
}

void AudioSdDiagnostic::fail(const char* reason) {
  if (state_ == State::kFailed) {
    return;
  }
  state_ = State::kFailed;
  Serial.print("MIC_SD_TEST_ERROR reason=");
  Serial.println(reason);
  if (audioFile_) {
    audioFile_.close();
  }
  digitalWrite(pins::kSdChipSelect, HIGH);
}

void AudioSdDiagnostic::buildPath(char* destination, size_t length,
                                  const char* filename) const {
  snprintf(destination, length, "%s/%s", folder_, filename);
}

void AudioSdDiagnostic::recordDuration(uint32_t durationUs,
                                       uint32_t& maximumUs,
                                       uint32_t& slowOperations) {
  if (durationUs > maximumUs) {
    maximumUs = durationUs;
  }
  if (durationUs >= config::kSdSlowOperationThresholdUs) {
    ++slowOperations;
  }
}

}  // namespace pet::services
