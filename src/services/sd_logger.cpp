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
  if (!SD.begin(pins::kSdChipSelect)) {
    confirmCardFailure("initialization");
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
  if (!openAudioFile()) {
    confirmCardFailure("audio_open");
    return false;
  }

  char imuPath[32]{};
  buildPath(imuPath, sizeof(imuPath), "imu.bin");
  SD.remove(imuPath);
  imuFile_ = SD.open(imuPath, FILE_WRITE);
  if (!imuFile_) {
    confirmCardFailure("imu_open");
    return false;
  }

  bufferHead_ = 0;
  bufferTail_ = 0;
  bufferedBytes_ = 0;
  audioBufferHead_ = 0;
  audioBufferTail_ = 0;
  audioBufferedBytes_ = 0;
  packetsAtLastFlush_ = 0;
  packetsAtLastStatus_ = 0;
  healthWindowStartedMs_ = millis();
  imuWriteAttemptedInWindow_ = false;
  imuWriteSucceededInWindow_ = false;
  audioWriteAttemptedInWindow_ = false;
  audioWriteSucceededInWindow_ = false;
  counters_ = {};
  sessionActive_ = true;

  const uint32_t statusStartedUs = micros();
  const bool statusWritten =
      writeStatus(acquisitionCounters, audioCounters, "recording");
  recordOperationDuration(micros() - statusStartedUs,
                          counters_.maxStatusDurationUs,
                          counters_.slowStatusUpdates);
  if (!statusWritten) {
    confirmCardFailure("initial_status");
    return false;
  }
  return true;
}

bool SdLogger::enqueue(const data::ImuPacket& packet) {
  if (!sessionActive_) {
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
  ++counters_.packetsQueued;
  return true;
}

bool SdLogger::canEnqueueAudioBlock() const {
  return sessionActive_ && sessionMetadata_.audioEnabled &&
         sizeof(AudioPcmBlock) <=
             config::kSdAudioRamBufferBytes - audioBufferedBytes_;
}

bool SdLogger::enqueueAudio(const AudioPcmBlock& block) {
  if (!canEnqueueAudioBlock()) {
    ++counters_.audioBlocksDropped;
    return false;
  }

  for (size_t i = 0; i < config::kMicrophoneBlockSamples; ++i) {
    const uint16_t sample = static_cast<uint16_t>(block.samples[i]);
    audioBuffer_[audioBufferTail_] = static_cast<uint8_t>(sample & 0xFFU);
    audioBufferTail_ =
        (audioBufferTail_ + 1U) % config::kSdAudioRamBufferBytes;
    audioBuffer_[audioBufferTail_] = static_cast<uint8_t>(sample >> 8U);
    audioBufferTail_ =
        (audioBufferTail_ + 1U) % config::kSdAudioRamBufferBytes;
  }
  audioBufferedBytes_ += sizeof(block);
  ++counters_.audioBlocksQueued;
  return true;
}

void SdLogger::service(const AcquisitionCounters& acquisitionCounters,
                       const AudioCaptureCounters& audioCounters) {
  if (!sessionActive_) {
    return;
  }

  const uint32_t nowMs = millis();
  if (static_cast<int32_t>(nowMs - nextWriteRetryMs_) >= 0) {
    if (audioBufferedBytes_ >= config::kSdWriteBlockBytes) {
      writeAudioBufferedBytes(false);
    }
    if (bufferedBytes_ >= config::kSdWriteBlockBytes) {
      writeBufferedBytes(false);
    } else if (counters_.packetsQueued - packetsAtLastFlush_ >=
               config::kSdPacketsPerFlush) {
      writeBufferedBytes(true);
    }
  }

  const bool flushDue =
      counters_.packetsQueued - packetsAtLastFlush_ >=
      config::kSdPacketsPerFlush;
  if (flushDue && audioBufferedBytes_ > 0 &&
      audioBufferedBytes_ < config::kSdWriteBlockBytes && sessionActive_) {
    writeAudioBufferedBytes(true);
  }
  if (flushDue && bufferedBytes_ == 0 && audioBufferedBytes_ == 0 &&
      sessionActive_) {
    const uint32_t flushStartedUs = micros();
    imuFile_.flush();
    recordOperationDuration(micros() - flushStartedUs,
                            counters_.maxFlushDurationUs,
                            counters_.slowFlushes);
    ++counters_.flushes;
    if (sessionMetadata_.audioEnabled) {
      const uint32_t audioFlushStartedUs = micros();
      audioFile_.flush();
      recordOperationDuration(micros() - audioFlushStartedUs,
                              counters_.maxAudioFlushDurationUs,
                              counters_.slowAudioFlushes);
      ++counters_.audioFlushes;
    }
    packetsAtLastFlush_ = counters_.packetsQueued;
  }

  const bool statusDue =
      counters_.packetsQueued - packetsAtLastStatus_ >=
      config::kSdPacketsPerStatusUpdate;
  if (statusDue && bufferedBytes_ == 0 && audioBufferedBytes_ == 0 &&
      sessionActive_) {
    const uint32_t statusStartedUs = micros();
    const bool statusWritten =
        writeStatus(acquisitionCounters, audioCounters, "recording");
    recordOperationDuration(micros() - statusStartedUs,
                            counters_.maxStatusDurationUs,
                            counters_.slowStatusUpdates);
    if (statusWritten) {
      packetsAtLastStatus_ = counters_.packetsQueued;
    } else {
      confirmCardFailure("status_update");
    }
  }

  checkHealthWindow();
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
  file.println("audio_timestamp_reference=estimated_first_sample_dma_block");
  file.print("audio_timestamp_uncertainty_us=");
  file.println((config::kMicrophoneBlockSamples * 1000000UL +
                config::kMicrophoneSampleRateHz - 1U) /
               config::kMicrophoneSampleRateHz);
  file.println("audio_preallocation_enabled=0");
  file.println(
      "audio_preallocation_reason=requires_graceful_truncate_on_shutdown");
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

bool SdLogger::openAudioFile() {
  char path[32]{};
  buildPath(path, sizeof(path), "audio.raw");
  SD.remove(path);
  if (!sessionMetadata_.audioEnabled) {
    File placeholder = SD.open(path, FILE_WRITE);
    if (!placeholder) {
      return false;
    }
    placeholder.close();
    return true;
  }
  audioFile_ = SD.open(path, FILE_WRITE);
  return static_cast<bool>(audioFile_);
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
  file.println(audioCounters.blocksReceived);
  file.print("audio_capture_blocks_dropped=");
  file.println(audioCounters.blocksDropped);
  file.print("audio_incomplete_blocks=");
  file.println(audioCounters.incompleteBlocks);
  file.print("audio_samples_received=");
  file.println(audioCounters.samplesReceived);
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
  file.print("sd_audio_blocks_queued=");
  file.println(counters_.audioBlocksQueued);
  file.print("sd_audio_blocks_dropped=");
  file.println(counters_.audioBlocksDropped);
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
  file.flush();
  file.close();

  SD.remove(statusPath);
  return SD.rename(temporaryPath, statusPath);
}

bool SdLogger::writeBufferedBytes(bool allowPartialBlock) {
  if (!sessionActive_ || bufferedBytes_ == 0 ||
      (!allowPartialBlock && bufferedBytes_ < config::kSdWriteBlockBytes)) {
    return false;
  }

  size_t count = bufferedBytes_;
  if (count > config::kSdWriteBlockBytes) {
    count = config::kSdWriteBlockBytes;
  }
  const size_t contiguous = config::kSdRamBufferBytes - bufferHead_;
  if (count > contiguous) {
    count = contiguous;
  }

  ++counters_.writeAttempts;
  imuWriteAttemptedInWindow_ = true;
  const uint32_t writeStartedUs = micros();
  const size_t written = imuFile_.write(&buffer_[bufferHead_], count);
  recordOperationDuration(micros() - writeStartedUs,
                          counters_.maxWriteDurationUs,
                          counters_.slowWrites);
  if (written > 0) {
    advanceBuffer(written);
    counters_.bytesWritten += written;
    ++counters_.writeSuccesses;
    imuWriteSucceededInWindow_ = true;
  }
  if (written != count) {
    ++counters_.writeFailures;
    nextWriteRetryMs_ = millis() + config::kSdWriteRetryMs;
    return false;
  }
  return true;
}

bool SdLogger::writeAudioBufferedBytes(bool allowPartialBlock) {
  if (!sessionActive_ || !sessionMetadata_.audioEnabled ||
      audioBufferedBytes_ == 0 ||
      (!allowPartialBlock &&
       audioBufferedBytes_ < config::kSdWriteBlockBytes)) {
    return false;
  }

  size_t count = audioBufferedBytes_;
  if (count > config::kSdWriteBlockBytes) {
    count = config::kSdWriteBlockBytes;
  }
  const size_t contiguous =
      config::kSdAudioRamBufferBytes - audioBufferHead_;
  if (count > contiguous) {
    count = contiguous;
  }

  ++counters_.audioWriteAttempts;
  audioWriteAttemptedInWindow_ = true;
  const uint32_t writeStartedUs = micros();
  const size_t written = audioFile_.write(&audioBuffer_[audioBufferHead_], count);
  recordOperationDuration(micros() - writeStartedUs,
                          counters_.maxAudioWriteDurationUs,
                          counters_.slowAudioWrites);
  if (written > 0) {
    advanceAudioBuffer(written);
    counters_.audioBytesWritten += written;
    ++counters_.audioWriteSuccesses;
    audioWriteSucceededInWindow_ = true;
  }
  if (written != count) {
    ++counters_.audioWriteFailures;
    nextWriteRetryMs_ = millis() + config::kSdWriteRetryMs;
    return false;
  }
  return true;
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

void SdLogger::checkHealthWindow() {
  const uint32_t nowMs = millis();
  if (nowMs - healthWindowStartedMs_ < config::kSdHealthWindowMs) {
    return;
  }
  if (imuWriteAttemptedInWindow_ && !imuWriteSucceededInWindow_) {
    confirmCardFailure("imu_write_health_window");
    return;
  }
  if (audioWriteAttemptedInWindow_ && !audioWriteSucceededInWindow_) {
    confirmCardFailure("audio_write_health_window");
    return;
  }
  healthWindowStartedMs_ = nowMs;
  imuWriteAttemptedInWindow_ = false;
  imuWriteSucceededInWindow_ = false;
  audioWriteAttemptedInWindow_ = false;
  audioWriteSucceededInWindow_ = false;
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
