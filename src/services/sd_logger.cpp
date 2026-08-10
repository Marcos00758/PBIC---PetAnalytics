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

  pinMode(pins::kSdChipSelect, OUTPUT);
  digitalWrite(pins::kSdChipSelect, HIGH);
  if (!SD.begin(pins::kSdChipSelect)) {
    return false;
  }

  cardReady_ = verifyReadWrite();
  return cardReady_;
}

bool SdLogger::beginSession(
    const SdSessionMetadata& metadata,
    const AcquisitionCounters& acquisitionCounters) {
  if (!cardReady_ || !chooseSessionNumber()) {
    return false;
  }

  if (!SD.mkdir(sessionFolder_) || !persistSessionNumber() ||
      !writeMetadata(metadata) || !createAudioPlaceholder()) {
    return false;
  }

  char imuPath[32]{};
  buildPath(imuPath, sizeof(imuPath), "imu.bin");
  SD.remove(imuPath);
  imuFile_ = SD.open(imuPath, FILE_WRITE);
  if (!imuFile_) {
    return false;
  }

  bufferHead_ = 0;
  bufferTail_ = 0;
  bufferedBytes_ = 0;
  packetsAtLastFlush_ = 0;
  packetsAtLastStatus_ = 0;
  healthWindowStartedMs_ = millis();
  writeAttemptedInWindow_ = false;
  writeSucceededInWindow_ = false;
  counters_ = {};
  sessionMetadata_ = metadata;
  sessionActive_ = true;

  if (!writeStatus(acquisitionCounters, "recording")) {
    handleCardFailure();
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

void SdLogger::service(const AcquisitionCounters& acquisitionCounters) {
  if (!sessionActive_) {
    return;
  }

  const uint32_t nowMs = millis();
  if (static_cast<int32_t>(nowMs - nextWriteRetryMs_) >= 0) {
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
  if (flushDue && bufferedBytes_ == 0 && sessionActive_) {
    imuFile_.flush();
    ++counters_.flushes;
    packetsAtLastFlush_ = counters_.packetsQueued;
  }

  const bool statusDue =
      counters_.packetsQueued - packetsAtLastStatus_ >=
      config::kSdPacketsPerStatusUpdate;
  if (statusDue && bufferedBytes_ == 0 && sessionActive_) {
    if (writeStatus(acquisitionCounters, "recording")) {
      packetsAtLastStatus_ = counters_.packetsQueued;
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

bool SdLogger::createAudioPlaceholder() {
  char path[32]{};
  buildPath(path, sizeof(path), "audio.raw");
  SD.remove(path);
  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    return false;
  }
  file.close();
  return true;
}

bool SdLogger::writeStatus(
    const AcquisitionCounters& acquisitionCounters, const char* state) {
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
  writeAttemptedInWindow_ = true;
  const size_t written = imuFile_.write(&buffer_[bufferHead_], count);
  if (written > 0) {
    advanceBuffer(written);
    counters_.bytesWritten += written;
    ++counters_.writeSuccesses;
    writeSucceededInWindow_ = true;
  }
  if (written != count) {
    ++counters_.writeFailures;
    nextWriteRetryMs_ = millis() + config::kSdWriteRetryMs;
    return false;
  }
  return true;
}

void SdLogger::advanceBuffer(size_t count) {
  bufferHead_ = (bufferHead_ + count) % config::kSdRamBufferBytes;
  bufferedBytes_ -= count;
}

void SdLogger::checkHealthWindow() {
  const uint32_t nowMs = millis();
  if (nowMs - healthWindowStartedMs_ < config::kSdHealthWindowMs) {
    return;
  }
  if (writeAttemptedInWindow_ && !writeSucceededInWindow_) {
    handleCardFailure();
    return;
  }
  healthWindowStartedMs_ = nowMs;
  writeAttemptedInWindow_ = false;
  writeSucceededInWindow_ = false;
}

void SdLogger::handleCardFailure() {
  sessionActive_ = false;
  cardReady_ = false;
  if (imuFile_) {
    imuFile_.close();
  }
}

void SdLogger::buildPath(char* destination, size_t length,
                         const char* filename) const {
  snprintf(destination, length, "%s/%s", sessionFolder_, filename);
}

}  // namespace pet::services
