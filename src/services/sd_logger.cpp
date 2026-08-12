#include "services/sd_logger.h"

#include <SPI.h>
#include <stdlib.h>
#include <string.h>

#include "config/constants.h"
#include "config/pins.h"

namespace pet::services {
namespace {

constexpr char kCardTestPath[] = "/pbic_sd_test.tmp";
constexpr char kSessionPath[] = "/session.txt";
constexpr char kSessionTempPath[] = "/session.tmp";
constexpr uint8_t kTestPattern[] = {0x50, 0x42, 0x49, 0x43, 0x53, 0x44};

uint32_t nominalRecordingBytesPerSecond() {
  uint32_t bytes = sizeof(data::ImuPacket) * config::kImuSampleRateHz;
  if (config::kMicrophoneRecordingEnabled) {
    bytes += config::kMicrophoneSampleRateHz * config::kMicrophoneChannels *
             (config::kMicrophoneBitsPerSample / 8U);
  }
  return bytes;
}

uint64_t imuPreallocationBytes() {
  return static_cast<uint64_t>(sizeof(data::ImuPacket)) *
         config::kImuSampleRateHz *
         (config::kSdSessionDurationSeconds +
          config::kSdPreallocationMarginSeconds);
}

uint64_t audioPreallocationBytes() {
  return static_cast<uint64_t>(config::kMicrophoneSampleRateHz) *
         config::kMicrophoneChannels *
         (config::kMicrophoneBitsPerSample / 8U) *
         (config::kSdSessionDurationSeconds +
          config::kSdPreallocationMarginSeconds);
}

void printHexByte(File& file, uint8_t value) {
  if (value < 0x10) {
    file.print('0');
  }
  file.print(value, HEX);
}

void printIndexedCounters(File& file, const char* prefix,
                          const uint32_t* values, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    file.print(prefix);
    file.print(i);
    file.print('=');
    file.println(values[i]);
  }
}

}  // namespace

bool SdLogger::beginCard() {
  cardReady_ = false;
  sessionActive_ = false;
  failureConfirmed_ = false;
  failureIndicatorReady_ = false;

  pinMode(pins::kSdChipSelect, OUTPUT);
  digitalWrite(pins::kSdChipSelect, HIGH);
  const SdSpiConfig spiConfig(pins::kSdChipSelect, SHARED_SPI,
                              SD_SCK_MHZ(config::kSdSpiClockMHz));
  if (!SD.sdfs.begin(spiConfig)) {
    confirmCardFailure("initialization");
    return false;
  }

  const uint32_t bytesPerCluster = SD.sdfs.bytesPerCluster();
  const uint32_t clusterCount = SD.sdfs.clusterCount();
  const uint32_t freeClusterCount = SD.sdfs.freeClusterCount();
  if (bytesPerCluster == 0 || clusterCount == 0 ||
      freeClusterCount > clusterCount) {
    confirmCardFailure("capacity_query");
    return false;
  }
  const uint64_t totalBytes =
      static_cast<uint64_t>(bytesPerCluster) * clusterCount;
  freeBytesAtBoot_ =
      static_cast<uint64_t>(bytesPerCluster) * freeClusterCount;
  const uint32_t bytesPerSecond = nominalRecordingBytesPerSecond();
  recordingBudgetBytes_ =
      freeBytesAtBoot_ > config::kSdFreeSpaceReserveBytes
          ? freeBytesAtBoot_ - config::kSdFreeSpaceReserveBytes
          : 0;
  estimatedRecordingSeconds_ = static_cast<uint32_t>(
      recordingBudgetBytes_ / bytesPerSecond);
  Serial.print("SD_CAPACITY total_bytes=");
  Serial.print(totalBytes);
  Serial.print(" free_bytes=");
  Serial.print(freeBytesAtBoot_);
  Serial.print(" reserve_bytes=");
  Serial.print(config::kSdFreeSpaceReserveBytes);
  Serial.print(" estimated_recording_seconds=");
  Serial.println(estimatedRecordingSeconds_);
  if (estimatedRecordingSeconds_ < config::kSdMinimumRecordingSeconds) {
    confirmCardFailure("insufficient_free_space");
    return false;
  }

  cardReady_ = verifyReadWrite();
  if (!cardReady_) {
    confirmCardFailure("read_write_diagnostic");
  }
  return cardReady_;
}

bool SdLogger::beginSession(
    const SdSessionMetadata& metadata,
    const AcquisitionCounters& acquisitionCounters,
    const AudioCaptureCounters& audioCounters) {
  if (!cardReady_) {
    return false;
  }
  sessionMetadata_ = metadata;
  audioCountersAtSessionStart_ = audioCounters;
  counters_ = {};
  imuPreallocatedBytes_ = imuPreallocationBytes();
  audioPreallocatedBytes_ =
      metadata.audioEnabled ? audioPreallocationBytes() : 0;
  if (!hasSpaceForNextSession()) {
    confirmCardFailure("preallocation_space");
    return false;
  }
  if (!chooseSessionNumber()) {
    confirmCardFailure("session_number");
    return false;
  }
  if (!SD.mkdir(sessionFolder_)) {
    confirmCardFailure("session_directory");
    return false;
  }
  if (!persistSessionNumber()) {
    confirmCardFailure("session_counter");
    return false;
  }
  if (!writeMetadata(metadata)) {
    confirmCardFailure("metadata");
    return false;
  }
  if (!openAndPreallocateDataFiles()) {
    confirmCardFailure("data_preallocation");
    return false;
  }
  bufferHead_ = 0;
  bufferTail_ = 0;
  bufferedBytes_ = 0;
  audioBufferHead_ = 0;
  audioBufferTail_ = 0;
  audioBufferedBytes_ = 0;
  packetsAtLastFlush_ = 0;
  packetsAtLastJournal_ = 0;
  packetsAtLastStatus_ = 0;
  imuDurableBytes_ = 0;
  audioDurableBytes_ = 0;
  audioSequenceInitialized_ = false;
  audioGapInProgress_ = false;
  imuFlushPending_ = false;
  audioFlushPending_ = false;
  journalPending_ = false;
  statusPending_ = false;
  finalizeStage_ = FinalizeStage::kIdle;
  imuWriteFailureStartedMs_ = 0;
  audioWriteFailureStartedMs_ = 0;
  imuWriteFailureActive_ = false;
  audioWriteFailureActive_ = false;
  sessionStartedMs_ = millis();
  audioFirstTimestampUs_ = metadata.audioStartTimestampUs;
  audioFirstTimestampValid_ = metadata.audioStartTimestampValid;
  stopRequested_ = false;
  stopRequestedAtMs_ = 0;
  stopReason_ = nullptr;
  sessionActive_ = true;

  const uint32_t statusStartedUs = micros();
  const bool statusWritten =
      writeStatus(acquisitionCounters, audioCounters, "recording");
  recordOperationDuration(micros() - statusStartedUs,
                          counters_.maxStatusDurationUs,
                          counters_.slowStatusUpdates);
  if (!statusWritten) {
    discardEmptyPreallocation();
    confirmCardFailure("initial_status");
    return false;
  }
  if (!writeJournal("recording")) {
    discardEmptyPreallocation();
    confirmCardFailure("initial_journal");
    return false;
  }
  return true;
}

bool SdLogger::finalizeInitialSessionSetup(
    const SdSessionMetadata& metadata,
    const AcquisitionCounters& acquisitionCounters,
    const AudioCaptureCounters& audioCounters) {
  if (!sessionActive_) {
    return false;
  }

  sessionMetadata_ = metadata;
  sessionStartedMs_ = millis();
  audioFirstTimestampUs_ = metadata.audioStartTimestampUs;
  audioFirstTimestampValid_ = metadata.audioStartTimestampValid;
  if (!metadata.audioEnabled) {
    if (audioFile_ && !audioFile_.truncate(0)) {
      confirmCardFailure("disabled_audio_truncate");
      return false;
    }
    audioPreallocatedBytes_ = 0;
  }
  if (!writeMetadata(metadata)) {
    confirmCardFailure("final_metadata");
    return false;
  }
  if (!writeStatus(acquisitionCounters, audioCounters, "recording")) {
    confirmCardFailure("final_initial_status");
    return false;
  }
  if (!writeJournal("recording")) {
    confirmCardFailure("final_initial_journal");
    return false;
  }
  return true;
}

bool SdLogger::enqueue(const data::ImuPacket& packet) {
  if (!sessionActive_ || stopRequested_) {
    return false;
  }
  if (sizeof(packet) > config::kSdRamBufferBytes - bufferedBytes_) {
    ++counters_.packetsDropped;
    return false;
  }

  const uint8_t* source = reinterpret_cast<const uint8_t*>(&packet);
  for (size_t i = 0; i < sizeof(packet); ++i) {
    buffer_[bufferTail_] = source[i];
    bufferTail_ = (bufferTail_ + 1U) % config::kSdRamBufferBytes;
  }
  bufferedBytes_ += sizeof(packet);
  if (bufferedBytes_ > counters_.maxBufferedBytes) {
    counters_.maxBufferedBytes = bufferedBytes_;
  }
  ++counters_.packetsQueued;
  return true;
}

bool SdLogger::canEnqueueAudioBlock() const {
  return sessionActive_ && !stopRequested_ && sessionMetadata_.audioEnabled &&
         kAudioPcmBytesPerBlock <=
             config::kSdAudioRamBufferBytes - audioBufferedBytes_;
}

bool SdLogger::enqueueAudio(const AudioPcmBlock& block) {
  if (!sessionActive_ || stopRequested_ ||
      !sessionMetadata_.audioEnabled) {
    return false;
  }

  if (!audioSequenceInitialized_) {
    nextAudioSequence_ = block.sequence;
    audioSequenceInitialized_ = true;
  }

  const uint32_t missingBlocks = block.sequence - nextAudioSequence_;
  if (missingBlocks != 0 && missingBlocks < 0x80000000UL) {
    if (!audioGapInProgress_) {
      audioGapInProgress_ = true;
      ++counters_.audioGapEvents;
      if (missingBlocks > counters_.maxAudioGapBlocks) {
        counters_.maxAudioGapBlocks = missingBlocks;
      }
    }
    static const int16_t silence[config::kMicrophoneBlockSamples]{};
    while (nextAudioSequence_ != block.sequence && canEnqueueAudioBlock()) {
      if (!appendAudioSamples(silence)) {
        return false;
      }
      ++nextAudioSequence_;
      ++counters_.audioSilenceBlocksInserted;
    }
    if (nextAudioSequence_ != block.sequence) {
      return false;
    }
  } else if (missingBlocks >= 0x80000000UL) {
    ++counters_.audioBlocksDropped;
    return true;
  }

  if (!canEnqueueAudioBlock()) {
    return false;
  }
  if (!audioFirstTimestampValid_) {
    audioFirstTimestampUs_ = block.timestampUs;
    audioFirstTimestampValid_ = block.timestampUs != 0;
  }
  if (!appendAudioSamples(block.samples)) {
    return false;
  }
  nextAudioSequence_ = block.sequence + 1U;
  audioGapInProgress_ = false;
  ++counters_.audioBlocksQueued;
  return true;
}

bool SdLogger::appendAudioSamples(const int16_t* samples) {
  if (kAudioPcmBytesPerBlock >
      config::kSdAudioRamBufferBytes - audioBufferedBytes_) {
    return false;
  }
  for (size_t i = 0; i < config::kMicrophoneBlockSamples; ++i) {
    const uint16_t sample = static_cast<uint16_t>(samples[i]);
    audioBuffer_[audioBufferTail_] = static_cast<uint8_t>(sample & 0xFFU);
    audioBufferTail_ =
        (audioBufferTail_ + 1U) % config::kSdAudioRamBufferBytes;
    audioBuffer_[audioBufferTail_] = static_cast<uint8_t>(sample >> 8U);
    audioBufferTail_ =
        (audioBufferTail_ + 1U) % config::kSdAudioRamBufferBytes;
  }
  audioBufferedBytes_ += kAudioPcmBytesPerBlock;
  if (audioBufferedBytes_ > counters_.maxAudioBufferedBytes) {
    counters_.maxAudioBufferedBytes = audioBufferedBytes_;
  }
  return true;
}

void SdLogger::service(const AcquisitionCounters& acquisitionCounters,
                       const AudioCaptureCounters& audioCounters) {
  if (!sessionActive_) {
    return;
  }

  const uint32_t nowMs = millis();
  if (!stopRequested_) {
    if (nowMs - sessionStartedMs_ >=
        config::kSdSessionDurationSeconds * 1000UL) {
      requestSessionStop("completed_duration");
    }

    if (!imuFlushPending_ && !audioFlushPending_ &&
        counters_.packetsQueued - packetsAtLastFlush_ >=
            config::kSdPacketsPerFlush) {
      imuFlushPending_ = true;
      audioFlushPending_ = sessionMetadata_.audioEnabled;
      packetsAtLastFlush_ = counters_.packetsQueued;
    }
    if (!journalPending_ &&
        ((packetsAtLastJournal_ == 0 &&
          counters_.packetsQueued >= config::kSdPacketsPerFlush) ||
         counters_.packetsQueued - packetsAtLastJournal_ >=
             config::kSdPacketsPerJournalUpdate)) {
      journalPending_ = true;
    }
    if (!statusPending_ &&
        counters_.packetsQueued - packetsAtLastStatus_ >=
            config::kSdPacketsPerStatusUpdate) {
      statusPending_ = true;
    }
  }

  bool operationPerformed = false;
  if (static_cast<int32_t>(nowMs - nextWriteRetryMs_) >= 0) {
    const bool imuReady =
        bufferedBytes_ >= config::kSdImuWriteBlockBytes ||
        (stopRequested_ && bufferedBytes_ > 0);
    const bool audioReady =
        audioBufferedBytes_ >= config::kSdAudioWriteBlockBytes ||
        (stopRequested_ && audioBufferedBytes_ > 0);
    bool chooseAudio = audioReady && !imuReady;
    if (imuReady && audioReady) {
      const bool audioUrgent =
          static_cast<uint64_t>(audioBufferedBytes_) * 100U >=
          static_cast<uint64_t>(config::kSdAudioRamBufferBytes) *
              config::kSdAudioUrgentPercent;
      const bool audioAtLeastAsFull =
          static_cast<uint64_t>(audioBufferedBytes_) *
              config::kSdRamBufferBytes >=
          static_cast<uint64_t>(bufferedBytes_) *
              config::kSdAudioRamBufferBytes;
      chooseAudio = audioUrgent || audioAtLeastAsFull;
      if (chooseAudio) {
        ++counters_.audioPriorityWrites;
      }
    }
    if (chooseAudio) {
      writeAudioBufferedBytes(
          stopRequested_ &&
          audioBufferedBytes_ < config::kSdAudioWriteBlockBytes);
      operationPerformed = true;
    } else if (imuReady) {
      writeBufferedBytes(stopRequested_ &&
                         bufferedBytes_ < config::kSdImuWriteBlockBytes);
      operationPerformed = true;
    }
  }

  if (!operationPerformed && stopRequested_ && bufferedBytes_ == 0 &&
      audioBufferedBytes_ == 0 && sessionActive_) {
    finishSession(acquisitionCounters, audioCounters);
    return;
  }

  if (!operationPerformed && !stopRequested_) {
    if (imuFlushPending_) {
      if (!flushImuFile()) {
        confirmCardFailure("imu_flush");
        return;
      }
      imuFlushPending_ = false;
      operationPerformed = true;
    } else if (audioFlushPending_) {
      if (!flushAudioFile()) {
        confirmCardFailure("audio_flush");
        return;
      }
      audioFlushPending_ = false;
      operationPerformed = true;
    } else if (journalPending_) {
      if (!writeJournal("recording")) {
        confirmCardFailure("journal_update");
        return;
      }
      packetsAtLastJournal_ = counters_.packetsQueued;
      journalPending_ = false;
      operationPerformed = true;
    } else if (statusPending_) {
      const uint32_t statusStartedUs = micros();
      const bool statusWritten =
          writeStatus(acquisitionCounters, audioCounters, "recording");
      recordOperationDuration(micros() - statusStartedUs,
                              counters_.maxStatusDurationUs,
                              counters_.slowStatusUpdates);
      if (!statusWritten) {
        confirmCardFailure("status_update");
        return;
      }
      packetsAtLastStatus_ = counters_.packetsQueued;
      statusPending_ = false;
    }
  }

  checkWriteFailureTimeout();
}

bool SdLogger::verifyReadWrite() {
  SD.remove(kCardTestPath);
  File output = SD.open(kCardTestPath, FILE_WRITE);
  if (!output) {
    return false;
  }
  const size_t written = output.write(kTestPattern, sizeof(kTestPattern));
  output.flush();
  output.close();
  if (written != sizeof(kTestPattern)) {
    SD.remove(kCardTestPath);
    return false;
  }

  uint8_t received[sizeof(kTestPattern)]{};
  File input = SD.open(kCardTestPath, FILE_READ);
  if (!input) {
    SD.remove(kCardTestPath);
    return false;
  }
  const int bytesRead = input.read(received, sizeof(received));
  input.close();
  SD.remove(kCardTestPath);
  return bytesRead == static_cast<int>(sizeof(received)) &&
         memcmp(received, kTestPattern, sizeof(received)) == 0;
}

bool SdLogger::chooseSessionNumber() {
  uint32_t lastSession = 0;
  File counter = SD.open(kSessionPath, FILE_READ);
  if (counter) {
    char text[16]{};
    const int count = counter.read(text, sizeof(text) - 1U);
    counter.close();
    if (count > 0) {
      lastSession = strtoul(text, nullptr, 10);
    }
  }

  for (uint32_t candidate = lastSession + 1U; candidate != 0; ++candidate) {
    snprintf(sessionFolder_, sizeof(sessionFolder_), "/S%03lu",
             static_cast<unsigned long>(candidate));
    if (!SD.exists(sessionFolder_)) {
      sessionNumber_ = candidate;
      return true;
    }
  }
  return false;
}

bool SdLogger::persistSessionNumber() {
  SD.remove(kSessionTempPath);
  File output = SD.open(kSessionTempPath, FILE_WRITE);
  if (!output) {
    return false;
  }
  output.println(sessionNumber_);
  output.flush();
  output.close();

  SD.remove(kSessionPath);
  return SD.rename(kSessionTempPath, kSessionPath);
}

bool SdLogger::writeMetadata(const SdSessionMetadata& metadata) {
  char path[32]{};
  buildPath(path, sizeof(path), "meta.txt");
  SD.remove(path);
  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    return false;
  }

  file.print("session=");
  file.println(sessionNumber_);
  file.print("boot_micros=");
  file.println(micros());
  file.print("firmware_version=");
  file.println(config::kFirmwareVersion);
  file.println("board=Teensy 4.0");
  file.print("sd_spi_clock_mhz=");
  file.println(config::kSdSpiClockMHz);
  file.print("sd_imu_write_block_bytes=");
  file.println(config::kSdImuWriteBlockBytes);
  file.print("sd_audio_write_block_bytes=");
  file.println(config::kSdAudioWriteBlockBytes);
  file.print("sd_free_bytes_at_boot=");
  file.println(freeBytesAtBoot_);
  file.print("sd_recording_budget_bytes=");
  file.println(recordingBudgetBytes_);
  file.print("sd_estimated_recording_seconds=");
  file.println(estimatedRecordingSeconds_);
  file.print("sd_session_duration_seconds=");
  file.println(config::kSdSessionDurationSeconds);
  file.print("sd_preallocation_margin_seconds=");
  file.println(config::kSdPreallocationMarginSeconds);
  file.print("imu_preallocated_bytes=");
  file.println(imuPreallocatedBytes_);
  file.print("audio_preallocated_bytes=");
  file.println(audioPreallocatedBytes_);
  file.print("journal_update_packets=");
  file.println(config::kSdPacketsPerJournalUpdate);
  file.print("packet_version=");
  file.println(config::kImuPacketVersion);
  file.print("packet_size=");
  file.println(sizeof(data::ImuPacket));
  file.println("endianness=little");
  file.println("magic=0xAA55");
  file.println("crc=CRC-8 polynomial 0x07");
  file.print("imu_sample_rate_hz=");
  file.println(config::kImuSampleRateHz);
  file.print("bmp_sample_rate_hz=");
  file.println(config::kBmpSampleRateHz);
  file.print("mag_sample_rate_hz=");
  file.println(config::kMagSampleRateHz);
  file.print("accel_range_g=");
  file.println(config::kIcmAccelRangeG);
  file.print("gyro_range_dps=");
  file.println(config::kIcmGyroRangeDps);
  file.print("audio_enabled=");
  file.println(metadata.audioEnabled ? 1 : 0);
  file.println("audio_format=pcm_s16le");
  file.println("audio_file=audio.raw");
  file.println("audio_byte_order=little");
  file.println("audio_dma=Teensy_AudioInputI2S");
  file.print("audio_sample_rate_hz=");
  file.println(config::kMicrophoneSampleRateHz);
  file.print("audio_channels=");
  file.println(config::kMicrophoneChannels);
  file.print("audio_bits_per_sample=");
  file.println(config::kMicrophoneBitsPerSample);
  file.print("audio_block_samples=");
  file.println(config::kMicrophoneBlockSamples);
  file.print("audio_capture_queue_usable_blocks=");
  file.println(config::kMicrophoneQueueBlocks - 1U);
  file.println("audio_gap_policy=zero_fill");
  file.println("audio_gap_detection=audio_dma_block_sequence");
  file.print("audio_sd_buffer_bytes=");
  file.println(config::kSdAudioRamBufferBytes);
  file.print("audio_nominal_bytes_per_second=");
  file.println(config::kMicrophoneSampleRateHz *
               config::kMicrophoneChannels *
               (config::kMicrophoneBitsPerSample / 8U));
  file.println("audio_i2s_channel=left");
  file.print("audio_start_timestamp_valid=");
  file.println(metadata.audioStartTimestampValid ? 1 : 0);
  file.print("audio_start_timestamp_us=");
  file.println(metadata.audioStartTimestampUs);
  file.print("audio_preflight_valid=");
  file.println(metadata.audioPreflight.valid ? 1 : 0);
  file.print("audio_preflight_accepted=");
  file.println(metadata.audioPreflight.accepted ? 1 : 0);
  file.print("audio_preflight_samples=");
  file.println(metadata.audioPreflight.samples);
  file.print("audio_preflight_mean_counts=");
  file.println(metadata.audioPreflight.meanCounts);
  file.print("audio_preflight_rms_counts=");
  file.println(metadata.audioPreflight.rmsCounts);
  file.print("audio_preflight_peak_counts=");
  file.println(metadata.audioPreflight.peakCounts);
  file.print("audio_preflight_clipping_samples=");
  file.println(metadata.audioPreflight.clippingSamples);
  file.println("audio_timestamp_reference=estimated_first_sample_dma_block");
  file.print("audio_timestamp_uncertainty_us=");
  file.println((config::kMicrophoneBlockSamples * 1000000UL +
                config::kMicrophoneSampleRateHz - 1U) /
               config::kMicrophoneSampleRateHz);
  file.println("preallocation_enabled=1");
  file.println("preallocation_tail_source=journal.txt");
  file.println("completed_sessions_truncated=1");
  file.print("icm0_channel=");
  file.println(config::kIcm0Channel);
  file.print("icm1_channel=");
  file.println(config::kIcm1Channel);
  file.print("icm2_channel=");
  file.println(config::kIcm2Channel);
  file.print("bmp0_channel=");
  file.println(config::kBmp0Channel);
  file.print("bmp1_channel=");
  file.println(config::kBmp1Channel);

  for (size_t i = 0; i < data::kIcmCount; ++i) {
    file.print("icm");
    file.print(i);
    file.print("_ready=");
    file.println(metadata.icmReady[i] ? 1 : 0);
  }

  for (size_t i = 0; i < data::kBmpCount; ++i) {
    file.print("bmp");
    file.print(i);
    file.print("_ready=");
    file.println(metadata.bmpReady[i] ? 1 : 0);
    file.print("bmp");
    file.print(i);
    file.print("_address=0x");
    printHexByte(file, metadata.bmpAddress[i]);
    file.println();
    file.print("bmp");
    file.print(i);
    file.print("_nvm_valid=");
    file.println(metadata.bmpNvmValid[i] ? 1 : 0);
    file.print("bmp");
    file.print(i);
    file.print("_nvm=");
    if (metadata.bmpNvmValid[i]) {
      for (size_t j = 0; j < drivers::kBmp390NvmLength; ++j) {
        printHexByte(file, metadata.bmpNvm[i][j]);
      }
    }
    file.println();
  }

  file.flush();
  file.close();
  return true;
}

bool SdLogger::hasSpaceForNextSession() {
  const uint32_t bytesPerCluster = SD.sdfs.bytesPerCluster();
  const uint32_t clusterCount = SD.sdfs.clusterCount();
  const uint32_t freeClusterCount = SD.sdfs.freeClusterCount();
  if (bytesPerCluster == 0 || clusterCount == 0 ||
      freeClusterCount > clusterCount) {
    return false;
  }
  const uint64_t freeBytes =
      static_cast<uint64_t>(bytesPerCluster) * freeClusterCount;
  return freeBytes >= imuPreallocatedBytes_ + audioPreallocatedBytes_ +
                          config::kSdFreeSpaceReserveBytes;
}

bool SdLogger::openAndPreallocateDataFiles() {
  char imuPath[32]{};
  char audioPath[32]{};
  buildPath(imuPath, sizeof(imuPath), "imu.bin");
  buildPath(audioPath, sizeof(audioPath), "audio.raw");
  SD.remove(imuPath);
  SD.remove(audioPath);

  const uint32_t startedUs = micros();
  imuFile_ = SD.sdfs.open(imuPath, O_RDWR | O_CREAT | O_TRUNC);
  if (!imuFile_ || !imuFile_.preAllocate(imuPreallocatedBytes_) ||
      !imuFile_.seekSet(0)) {
    if (imuFile_) {
      imuFile_.truncate(0);
      imuFile_.close();
    }
    SD.remove(imuPath);
    return false;
  }

  if (sessionMetadata_.audioEnabled) {
    audioFile_ = SD.sdfs.open(audioPath, O_RDWR | O_CREAT | O_TRUNC);
    if (!audioFile_ || !audioFile_.preAllocate(audioPreallocatedBytes_) ||
        !audioFile_.seekSet(0)) {
      if (audioFile_) {
        audioFile_.truncate(0);
        audioFile_.close();
      }
      imuFile_.truncate(0);
      imuFile_.close();
      SD.remove(audioPath);
      SD.remove(imuPath);
      return false;
    }
  } else {
    audioFile_ = SD.sdfs.open(audioPath, O_RDWR | O_CREAT | O_TRUNC);
    if (!audioFile_) {
      imuFile_.truncate(0);
      imuFile_.close();
      SD.remove(imuPath);
      return false;
    }
  }

  const uint32_t durationUs = micros() - startedUs;
  if (durationUs > counters_.maxPreallocationDurationUs) {
    counters_.maxPreallocationDurationUs = durationUs;
  }
  Serial.print("SD_PREALLOCATE folder=");
  Serial.print(sessionFolder_);
  Serial.print(" imu_bytes=");
  Serial.print(imuPreallocatedBytes_);
  Serial.print(" audio_bytes=");
  Serial.print(audioPreallocatedBytes_);
  Serial.print(" duration_us=");
  Serial.println(durationUs);
  return true;
}

bool SdLogger::writeJournal(const char* state) {
  char journalPath[32]{};
  char temporaryPath[32]{};
  buildPath(journalPath, sizeof(journalPath), "journal.txt");
  buildPath(temporaryPath, sizeof(temporaryPath), "journal.tmp");
  SD.remove(temporaryPath);

  const uint32_t startedUs = micros();
  File file = SD.open(temporaryPath, FILE_WRITE);
  if (!file) {
    return false;
  }
  file.print("state=");
  file.println(state);
  file.print("session=");
  file.println(sessionNumber_);
  file.print("uptime_ms=");
  file.println(millis());
  file.print("imu_valid_bytes=");
  file.println(imuDurableBytes_);
  file.print("audio_valid_bytes=");
  file.println(audioDurableBytes_);
  file.print("audio_silence_blocks_inserted=");
  file.println(counters_.audioSilenceBlocksInserted);
  file.print("audio_gap_events=");
  file.println(counters_.audioGapEvents);
  file.print("audio_max_gap_blocks=");
  file.println(counters_.maxAudioGapBlocks);
  file.print("audio_start_timestamp_valid=");
  file.println(audioFirstTimestampValid_ ? 1 : 0);
  file.print("audio_start_timestamp_us=");
  file.println(audioFirstTimestampUs_);
  file.print("imu_preallocated_bytes=");
  file.println(imuPreallocatedBytes_);
  file.print("audio_preallocated_bytes=");
  file.println(audioPreallocatedBytes_);
  file.flush();
  file.close();
  SD.remove(journalPath);
  if (!SD.rename(temporaryPath, journalPath)) {
    return false;
  }

  const uint32_t durationUs = micros() - startedUs;
  recordOperationDuration(durationUs, counters_.maxJournalDurationUs,
                          counters_.slowJournalUpdates);
  ++counters_.journalUpdates;
  return true;
}

bool SdLogger::writeStatus(
    const AcquisitionCounters& acquisitionCounters,
    const AudioCaptureCounters& audioCounters, const char* state) {
  char statusPath[32]{};
  char temporaryPath[32]{};
  buildPath(statusPath, sizeof(statusPath), "status.txt");
  buildPath(temporaryPath, sizeof(temporaryPath), "status.tmp");
  SD.remove(temporaryPath);

  File file = SD.open(temporaryPath, FILE_WRITE);
  if (!file) {
    return false;
  }
  file.print("state=");
  file.println(state);
  file.print("uptime_ms=");
  file.println(millis());
  file.print("packets_produced=");
  file.println(acquisitionCounters.packetsProduced);
  file.print("missed_schedule_readings=");
  file.println(acquisitionCounters.missedScheduleReadings);
  file.print("failed_acquisition_rounds=");
  file.println(acquisitionCounters.failedAcquisitionRounds);
  for (size_t i = 0; i < data::kIcmCount; ++i) {
    file.print("icm");
    file.print(i);
    file.print("_ready=");
    file.println(sessionMetadata_.icmReady[i] ? 1 : 0);
  }
  for (size_t i = 0; i < data::kBmpCount; ++i) {
    file.print("bmp");
    file.print(i);
    file.print("_ready=");
    file.println(sessionMetadata_.bmpReady[i] ? 1 : 0);
    file.print("bmp");
    file.print(i);
    file.print("_nvm_valid=");
    file.println(sessionMetadata_.bmpNvmValid[i] ? 1 : 0);
  }
  printIndexedCounters(file, "icm_i2c_failures_",
                       acquisitionCounters.i2cFailures, data::kIcmCount);
  printIndexedCounters(file, "bmp_i2c_failures_",
                       acquisitionCounters.bmpI2cFailures, data::kBmpCount);
  printIndexedCounters(file, "bmp_updates_", acquisitionCounters.bmpUpdates,
                       data::kBmpCount);
  printIndexedCounters(file, "mag_i2c_failures_",
                       acquisitionCounters.magI2cFailures, data::kIcmCount);
  printIndexedCounters(file, "mag_updates_", acquisitionCounters.magUpdates,
                       data::kIcmCount);
  printIndexedCounters(file, "mag_no_new_data_",
                       acquisitionCounters.magNoNewData, data::kIcmCount);
  printIndexedCounters(file, "mag_overruns_",
                       acquisitionCounters.magDataOverruns, data::kIcmCount);
  printIndexedCounters(file, "mag_overflows_",
                       acquisitionCounters.magOverflows, data::kIcmCount);
  file.print("usb_dropped_packets=");
  file.println(acquisitionCounters.usbDroppedPackets);
  file.print("audio_blocks_received=");
  file.println(audioCounters.blocksReceived -
               audioCountersAtSessionStart_.blocksReceived);
  file.print("audio_capture_blocks_dropped=");
  file.println(audioCounters.blocksDropped -
               audioCountersAtSessionStart_.blocksDropped);
  file.print("audio_incomplete_blocks=");
  file.println(audioCounters.incompleteBlocks -
               audioCountersAtSessionStart_.incompleteBlocks);
  file.print("audio_samples_received=");
  file.println((audioCounters.blocksReceived -
                audioCountersAtSessionStart_.blocksReceived) *
               config::kMicrophoneBlockSamples);
  file.print("audio_capture_queue_high_water_blocks=");
  file.println(audioCounters.queueHighWaterBlocks);
  file.print("sd_packets_queued=");
  file.println(counters_.packetsQueued);
  file.print("sd_packets_dropped=");
  file.println(counters_.packetsDropped);
  file.print("sd_bytes_written=");
  file.println(counters_.bytesWritten);
  file.print("sd_buffered_bytes=");
  file.println(bufferedBytes_);
  file.print("sd_write_attempts=");
  file.println(counters_.writeAttempts);
  file.print("sd_write_successes=");
  file.println(counters_.writeSuccesses);
  file.print("sd_write_failures=");
  file.println(counters_.writeFailures);
  file.print("sd_partial_writes=");
  file.println(counters_.partialWrites);
  file.print("sd_flushes=");
  file.println(counters_.flushes);
  file.print("sd_max_write_duration_us=");
  file.println(counters_.maxWriteDurationUs);
  file.print("sd_max_flush_duration_us=");
  file.println(counters_.maxFlushDurationUs);
  file.print("sd_max_status_duration_us=");
  file.println(counters_.maxStatusDurationUs);
  file.print("sd_slow_writes_over_10ms=");
  file.println(counters_.slowWrites);
  file.print("sd_slow_flushes_over_10ms=");
  file.println(counters_.slowFlushes);
  file.print("sd_slow_status_updates_over_10ms=");
  file.println(counters_.slowStatusUpdates);
  file.print("sd_max_buffered_bytes=");
  file.println(counters_.maxBufferedBytes);
  file.print("sd_audio_blocks_queued=");
  file.println(counters_.audioBlocksQueued);
  file.print("sd_audio_blocks_dropped=");
  file.println(counters_.audioBlocksDropped);
  file.print("sd_audio_silence_blocks_inserted=");
  file.println(counters_.audioSilenceBlocksInserted);
  file.print("sd_audio_gap_events=");
  file.println(counters_.audioGapEvents);
  file.print("sd_audio_max_gap_blocks=");
  file.println(counters_.maxAudioGapBlocks);
  file.print("sd_audio_bytes_written=");
  file.println(counters_.audioBytesWritten);
  file.print("sd_audio_buffered_bytes=");
  file.println(audioBufferedBytes_);
  file.print("sd_audio_write_attempts=");
  file.println(counters_.audioWriteAttempts);
  file.print("sd_audio_write_successes=");
  file.println(counters_.audioWriteSuccesses);
  file.print("sd_audio_write_failures=");
  file.println(counters_.audioWriteFailures);
  file.print("sd_audio_partial_writes=");
  file.println(counters_.audioPartialWrites);
  file.print("sd_audio_priority_writes=");
  file.println(counters_.audioPriorityWrites);
  file.print("sd_audio_flushes=");
  file.println(counters_.audioFlushes);
  file.print("sd_audio_max_write_duration_us=");
  file.println(counters_.maxAudioWriteDurationUs);
  file.print("sd_audio_max_flush_duration_us=");
  file.println(counters_.maxAudioFlushDurationUs);
  file.print("sd_audio_slow_writes_over_10ms=");
  file.println(counters_.slowAudioWrites);
  file.print("sd_audio_slow_flushes_over_10ms=");
  file.println(counters_.slowAudioFlushes);
  file.print("sd_audio_max_buffered_bytes=");
  file.println(counters_.maxAudioBufferedBytes);
  file.print("sd_journal_updates=");
  file.println(counters_.journalUpdates);
  file.print("sd_max_journal_duration_us=");
  file.println(counters_.maxJournalDurationUs);
  file.print("sd_slow_journal_updates_over_10ms=");
  file.println(counters_.slowJournalUpdates);
  file.print("sd_preallocation_duration_us=");
  file.println(counters_.maxPreallocationDurationUs);
  file.print("imu_preallocated_bytes=");
  file.println(imuPreallocatedBytes_);
  file.print("audio_preallocated_bytes=");
  file.println(audioPreallocatedBytes_);
  file.print("audio_session_start_timestamp_valid=");
  file.println(audioFirstTimestampValid_ ? 1 : 0);
  file.print("audio_session_start_timestamp_us=");
  file.println(audioFirstTimestampUs_);
  file.flush();
  file.close();

  SD.remove(statusPath);
  return SD.rename(temporaryPath, statusPath);
}

bool SdLogger::writeBufferedBytes(bool allowPartialBlock) {
  if (!sessionActive_ || bufferedBytes_ == 0 ||
      (!allowPartialBlock &&
       bufferedBytes_ < config::kSdImuWriteBlockBytes)) {
    return false;
  }

  const size_t count =
      allowPartialBlock ? bufferedBytes_ : config::kSdImuWriteBlockBytes;
  const size_t contiguous = config::kSdRamBufferBytes - bufferHead_;
  const uint8_t* source = &buffer_[bufferHead_];
  if (count > contiguous) {
    memcpy(writeScratch_, source, contiguous);
    memcpy(writeScratch_ + contiguous, buffer_, count - contiguous);
    source = writeScratch_;
  }
  if (count < config::kSdImuWriteBlockBytes) {
    ++counters_.partialWrites;
  }

  ++counters_.writeAttempts;
  const uint32_t writeStartedUs = micros();
  const size_t written = imuFile_.write(source, count);
  recordOperationDuration(micros() - writeStartedUs,
                          counters_.maxWriteDurationUs,
                          counters_.slowWrites);
  if (written > 0) {
    advanceBuffer(written);
    counters_.bytesWritten += written;
    ++counters_.writeSuccesses;
    imuWriteFailureActive_ = false;
  }
  if (written != count) {
    ++counters_.writeFailures;
    if (written == 0 && !imuWriteFailureActive_) {
      imuWriteFailureStartedMs_ = millis();
      imuWriteFailureActive_ = true;
    }
    nextWriteRetryMs_ = millis() + config::kSdWriteRetryMs;
    return false;
  }
  return true;
}

bool SdLogger::writeAudioBufferedBytes(bool allowPartialBlock) {
  if (!sessionActive_ || !sessionMetadata_.audioEnabled ||
      audioBufferedBytes_ == 0 ||
      (!allowPartialBlock &&
       audioBufferedBytes_ < config::kSdAudioWriteBlockBytes)) {
    return false;
  }

  const size_t count = allowPartialBlock ? audioBufferedBytes_
                                         : config::kSdAudioWriteBlockBytes;
  const size_t contiguous =
      config::kSdAudioRamBufferBytes - audioBufferHead_;
  const uint8_t* source = &audioBuffer_[audioBufferHead_];
  if (count > contiguous) {
    memcpy(writeScratch_, source, contiguous);
    memcpy(writeScratch_ + contiguous, audioBuffer_, count - contiguous);
    source = writeScratch_;
  }
  if (count < config::kSdAudioWriteBlockBytes) {
    ++counters_.audioPartialWrites;
  }

  ++counters_.audioWriteAttempts;
  const uint32_t writeStartedUs = micros();
  const size_t written = audioFile_.write(source, count);
  recordOperationDuration(micros() - writeStartedUs,
                          counters_.maxAudioWriteDurationUs,
                          counters_.slowAudioWrites);
  if (written > 0) {
    advanceAudioBuffer(written);
    counters_.audioBytesWritten += written;
    ++counters_.audioWriteSuccesses;
    audioWriteFailureActive_ = false;
  }
  if (written != count) {
    ++counters_.audioWriteFailures;
    if (written == 0 && !audioWriteFailureActive_) {
      audioWriteFailureStartedMs_ = millis();
      audioWriteFailureActive_ = true;
    }
    nextWriteRetryMs_ = millis() + config::kSdWriteRetryMs;
    return false;
  }
  return true;
}

bool SdLogger::flushImuFile() {
  const uint32_t startedUs = micros();
  const bool success = imuFile_.sync();
  recordOperationDuration(micros() - startedUs,
                          counters_.maxFlushDurationUs,
                          counters_.slowFlushes);
  ++counters_.flushes;
  if (success) {
    imuDurableBytes_ = counters_.bytesWritten;
  }
  return success;
}

bool SdLogger::flushAudioFile() {
  if (!sessionMetadata_.audioEnabled) {
    audioDurableBytes_ = 0;
    return true;
  }
  const uint32_t startedUs = micros();
  const bool success = audioFile_.sync();
  recordOperationDuration(micros() - startedUs,
                          counters_.maxAudioFlushDurationUs,
                          counters_.slowAudioFlushes);
  ++counters_.audioFlushes;
  if (success) {
    audioDurableBytes_ = counters_.audioBytesWritten;
  }
  return success;
}

void SdLogger::discardEmptyPreallocation() {
  if (imuFile_) {
    imuFile_.truncate(0);
    imuFile_.close();
  }
  if (audioFile_) {
    audioFile_.truncate(0);
    audioFile_.close();
  }
}

void SdLogger::advanceBuffer(size_t count) {
  bufferHead_ = (bufferHead_ + count) % config::kSdRamBufferBytes;
  bufferedBytes_ -= count;
}

void SdLogger::advanceAudioBuffer(size_t count) {
  audioBufferHead_ =
      (audioBufferHead_ + count) % config::kSdAudioRamBufferBytes;
  audioBufferedBytes_ -= count;
}

void SdLogger::checkWriteFailureTimeout() {
  const uint32_t nowMs = millis();
  if (imuWriteFailureActive_ &&
      nowMs - imuWriteFailureStartedMs_ >= config::kSdHealthWindowMs) {
    confirmCardFailure("imu_write_health_window");
    return;
  }
  if (audioWriteFailureActive_ &&
      nowMs - audioWriteFailureStartedMs_ >= config::kSdHealthWindowMs) {
    confirmCardFailure("audio_write_health_window");
  }
}

void SdLogger::requestSessionStop(const char* reason) {
  if (stopRequested_ || !sessionActive_) {
    return;
  }
  stopRequested_ = true;
  stopRequestedAtMs_ = millis();
  stopReason_ = reason;
  finalizeStage_ = FinalizeStage::kFlushImu;
  Serial.print("SD_SESSION_STOP_REQUESTED reason=");
  Serial.print(reason);
  Serial.print(" imu_buffered_bytes=");
  Serial.print(bufferedBytes_);
  Serial.print(" audio_buffered_bytes=");
  Serial.println(audioBufferedBytes_);
}

void SdLogger::finishSession(
    const AcquisitionCounters& acquisitionCounters,
    const AudioCaptureCounters& audioCounters) {
  const char* finalState = stopReason_ != nullptr ? stopReason_ : "stopped";
  switch (finalizeStage_) {
    case FinalizeStage::kFlushImu:
      if (!flushImuFile()) {
        confirmCardFailure("final_imu_flush");
        return;
      }
      finalizeStage_ = FinalizeStage::kFlushAudio;
      return;
    case FinalizeStage::kFlushAudio:
      if (!flushAudioFile()) {
        confirmCardFailure("final_audio_flush");
        return;
      }
      finalizeStage_ = FinalizeStage::kTruncateImu;
      return;
    case FinalizeStage::kTruncateImu:
      if (!imuFile_.truncate(counters_.bytesWritten)) {
        confirmCardFailure("imu_session_truncate");
        return;
      }
      finalizeStage_ = FinalizeStage::kTruncateAudio;
      return;
    case FinalizeStage::kTruncateAudio:
      if (sessionMetadata_.audioEnabled &&
          !audioFile_.truncate(counters_.audioBytesWritten)) {
        confirmCardFailure("audio_session_truncate");
        return;
      }
      finalizeStage_ = FinalizeStage::kJournal;
      return;
    case FinalizeStage::kJournal:
      if (!writeJournal(finalState)) {
        confirmCardFailure("final_journal");
        return;
      }
      finalizeStage_ = FinalizeStage::kStatus;
      return;
    case FinalizeStage::kStatus:
      if (!writeStatus(acquisitionCounters, audioCounters, finalState)) {
        confirmCardFailure("final_status");
        return;
      }
      finalizeStage_ = FinalizeStage::kCloseAndRotate;
      return;
    case FinalizeStage::kCloseAndRotate: {
      const bool rotate =
          strcmp(finalState, "completed_duration") == 0 && cardReady_;
      const uint32_t nextSessionBoundaryMs = stopRequestedAtMs_;
      SdSessionMetadata nextMetadata = sessionMetadata_;
      nextMetadata.audioStartTimestampValid = false;
      nextMetadata.audioStartTimestampUs = 0;
      imuFile_.close();
      if (audioFile_) {
        audioFile_.close();
      }
      sessionActive_ = false;
      Serial.print("SD_SESSION_STOPPED reason=");
      Serial.print(finalState);
      Serial.print(" imu_bytes=");
      Serial.print(counters_.bytesWritten);
      Serial.print(" audio_bytes=");
      Serial.println(counters_.audioBytesWritten);

      if (rotate) {
        if (beginSession(nextMetadata, acquisitionCounters, audioCounters)) {
          sessionStartedMs_ = nextSessionBoundaryMs;
          Serial.print("SD_SESSION_ROTATED folder=");
          Serial.println(sessionFolder_);
        } else if (!failureConfirmed_) {
          Serial.println(
              "SD_SESSION_ROTATION_STOPPED next_session_unavailable=1");
        }
      }
      return;
    }
    case FinalizeStage::kIdle:
      return;
  }
}

void SdLogger::confirmCardFailure(const char* reason) {
  if (failureConfirmed_) {
    return;
  }
  failureConfirmed_ = true;
  failureConfirmedAtMs_ = millis();
  sessionActive_ = false;
  cardReady_ = false;
  Serial.print("SD_ERROR_CONFIRMED reason=");
  Serial.print(reason);
  Serial.print(" recording_disabled=1 reboot_required=1 led_delay_ms=");
  Serial.println(config::kSdFailureLedDelayMs);
  const uint32_t nowMs = millis();
  Serial.print("SD_ERROR_STATE imu_attempts=");
  Serial.print(counters_.writeAttempts);
  Serial.print(" imu_successes=");
  Serial.print(counters_.writeSuccesses);
  Serial.print(" imu_failures=");
  Serial.print(counters_.writeFailures);
  Serial.print(" imu_buffered_bytes=");
  Serial.print(bufferedBytes_);
  Serial.print(" imu_failure_age_ms=");
  Serial.print(imuWriteFailureActive_ ? nowMs - imuWriteFailureStartedMs_ : 0);
  Serial.print(" audio_attempts=");
  Serial.print(counters_.audioWriteAttempts);
  Serial.print(" audio_successes=");
  Serial.print(counters_.audioWriteSuccesses);
  Serial.print(" audio_failures=");
  Serial.print(counters_.audioWriteFailures);
  Serial.print(" audio_buffered_bytes=");
  Serial.print(audioBufferedBytes_);
  Serial.print(" audio_failure_age_ms=");
  Serial.print(audioWriteFailureActive_ ? nowMs - audioWriteFailureStartedMs_
                                        : 0);
  Serial.print(" max_imu_write_us=");
  Serial.print(counters_.maxWriteDurationUs);
  Serial.print(" max_audio_write_us=");
  Serial.println(counters_.maxAudioWriteDurationUs);
  if (imuFile_) {
    imuFile_.close();
  }
  if (audioFile_) {
    audioFile_.close();
  }
  digitalWrite(pins::kSdChipSelect, HIGH);
}

void SdLogger::updateFailureIndicator() {
  if (!failureConfirmed_ || !config::kSdFailureIndicatorEnabled) {
    return;
  }

  const uint32_t elapsedMs = millis() - failureConfirmedAtMs_;
  if (elapsedMs < config::kSdFailureLedDelayMs) {
    return;
  }
  if (!failureIndicatorReady_) {
    digitalWrite(pins::kSdChipSelect, HIGH);
    SPI.end();
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    failureIndicatorReady_ = true;
    Serial.println("SD_ERROR_SIGNAL led=orange_blink spi=disabled");
  }

  const uint32_t phaseMs =
      (elapsedMs - config::kSdFailureLedDelayMs) %
      config::kSdFailureLedCycleMs;
  const bool ledOn =
      phaseMs < config::kSdFailureLedPulseMs ||
      (phaseMs >= config::kSdFailureLedSecondPulseMs &&
       phaseMs < config::kSdFailureLedSecondPulseMs +
                     config::kSdFailureLedPulseMs);
  digitalWrite(LED_BUILTIN, ledOn ? HIGH : LOW);
}

void SdLogger::recordOperationDuration(uint32_t durationUs,
                                       uint32_t& maximumUs,
                                       uint32_t& slowOperations) {
  if (durationUs > maximumUs) {
    maximumUs = durationUs;
  }
  if (durationUs >= config::kSdSlowOperationThresholdUs) {
    ++slowOperations;
  }
}

void SdLogger::buildPath(char* destination, size_t length,
                         const char* filename) const {
  snprintf(destination, length, "%s/%s", sessionFolder_, filename);
}

}  // namespace pet::services
