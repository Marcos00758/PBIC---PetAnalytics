#include "services/presentation_stream.h"

#include <Wire.h>

#include "config/constants.h"
#include "config/pins.h"

namespace pet::services {
namespace {

void printHexByte(uint8_t value) {
  if (value < 0x10) {
    Serial.print('0');
  }
  Serial.print(value, HEX);
}

const char* stageName(drivers::Bmp390InitStage stage) {
  switch (stage) {
    case drivers::Bmp390InitStage::kNotAttempted:
      return "not_attempted";
    case drivers::Bmp390InitStage::kChannelSelect:
      return "channel_select";
    case drivers::Bmp390InitStage::kAddressProbe:
      return "address_probe";
    case drivers::Bmp390InitStage::kLibraryBegin:
      return "library_begin";
    case drivers::Bmp390InitStage::kChipId:
      return "chip_id";
    case drivers::Bmp390InitStage::kReady:
      return "ready";
  }
  return "unknown";
}

}  // namespace

PresentationStream::PresentationStream(drivers::Pca9548a& mux,
                                       drivers::Icm20948& icm0,
                                       drivers::Icm20948& icm1,
                                       drivers::Icm20948& icm2,
                                       drivers::Bmp390& bmp0,
                                       drivers::Bmp390& bmp1,
                                       ImuAcquisition& acquisition)
    : mux_(mux),
      icms_{&icm0, &icm1, &icm2},
      bmps_{&bmp0, &bmp1},
      acquisition_(acquisition) {}

bool PresentationStream::begin() {
  streaming_ = false;
  errorReason_ = nullptr;
  scanned_ = false;
  nextBannerMs_ = millis();

  Wire.begin();
  Wire.setClock(config::kI2cClockHz);
  if (!mux_.begin()) {
    errorReason_ = "pca9548a_absent";
    printBanner();
    return false;
  }

  const bool sensorsReady = initializeSensors();
  if (!sensorsReady) {
    // Only probe the whole bus after a failure. Sweeping every address costs
    // hundreds of unanswered transactions and can leave the I2C peripheral
    // stuck, so it must never run ahead of a healthy initialization.
    scanBus();
    errorReason_ = "sensor_initialization";
    mux_.disableAllChannels();
    printBanner();
    return false;
  }
  mux_.disableAllChannels();

  // Set before printing: the first banner must already advertise the stream,
  // otherwise a host that reads only that one concludes no packets are coming.
  streaming_ = true;
  printBanner();
  acquisition_.start(micros());
  return true;
}

// Records which addresses answer on every multiplexer channel. The result is
// captured once and replayed with the banner, so a late USB connection still
// learns where each device actually sits. Each channel sweep ends with a fresh
// I2C peripheral: a full sweep produces around a hundred unanswered
// transactions, and without the reset a later channel could report an empty
// bus that is merely stuck.
void PresentationStream::scanBus() {
  scanned_ = true;
  for (uint8_t channel = 0; channel < kMuxChannelCount; ++channel) {
    scanCount_[channel] = 0;
    resetBus();
    if (!mux_.selectChannel(channel)) {
      continue;
    }
    for (uint8_t address = 0x08; address <= 0x77; ++address) {
      if (address == config::kPca9548aAddress) {
        continue;
      }
      Wire.beginTransmission(address);
      if (Wire.endTransmission() != 0) {
        continue;
      }
      if (scanCount_[channel] < kMaxScanAddresses) {
        scanAddresses_[channel][scanCount_[channel]++] = address;
      }
    }
  }
  resetBus();
}

void PresentationStream::resetBus() const {
  Wire.end();
  Wire.begin();
  Wire.setClock(config::kI2cClockHz);
}

void PresentationStream::service() {
  if (static_cast<int32_t>(millis() - nextBannerMs_) >= 0) {
    printBanner();
    nextBannerMs_ = millis() + config::kPresentationBannerIntervalMs;
  }
  if (!streaming_) {
    return;
  }

  data::ImuPacket packet{};
  if (!acquisition_.poll(packet)) {
    return;
  }
  if (Serial && Serial.availableForWrite() >=
                    static_cast<int>(sizeof(data::ImuPacket))) {
    Serial.write(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
  } else {
    acquisition_.recordUsbDrop();
  }
}

bool PresentationStream::initializeSensors() {
  bool ready = true;
  for (size_t i = 0; i < data::kIcmCount; ++i) {
    icmReady_[i] = icms_[i]->begin();
    ready = icmReady_[i] && ready;
  }
  for (size_t i = 0; i < data::kBmpCount; ++i) {
    // Kept apart so the banner can say whether detection or the sampling
    // configuration failed; collapsing both into one flag hides the cause.
    bmpBeginOk_[i] = bmps_[i]->begin();
    bmpSamplingOk_[i] = bmpBeginOk_[i] && bmps_[i]->startRawSampling25Hz();
    bmpReady_[i] = bmpSamplingOk_[i];
    if (bmpReady_[i]) {
      bmpNvmValid_[i] = bmps_[i]->readNvm(bmpNvm_[i]);
    }
    ready = bmpReady_[i] && ready;
  }
  return ready;
}

void PresentationStream::printBanner() {
  if (!Serial) {
    return;
  }
  printHeaderLine();
  // Only report a sweep that actually ran; empty lines would read as an empty
  // bus when the truth is that no scan happened.
  if (scanned_) {
    for (uint8_t channel = 0; channel < kMuxChannelCount; ++channel) {
      printScanLine(channel);
    }
  }
  for (size_t i = 0; i < data::kIcmCount; ++i) {
    printIcmLine(i);
  }
  for (size_t i = 0; i < data::kBmpCount; ++i) {
    printBmpLine(i);
    printBmpNvmLine(i);
  }
  if (errorReason_ != nullptr) {
    Serial.print("PRESENTATION_ERROR reason=");
    Serial.println(errorReason_);
  }
}

void PresentationStream::printScanLine(uint8_t channel) const {
  Serial.print("PRESENTATION_I2C_SCAN channel=");
  Serial.print(channel);
  Serial.print(" addresses=");
  for (uint8_t i = 0; i < scanCount_[channel]; ++i) {
    if (i > 0) {
      Serial.print(',');
    }
    Serial.print("0x");
    printHexByte(scanAddresses_[channel][i]);
  }
  Serial.println();
}

void PresentationStream::printHeaderLine() const {
  Serial.print("PRESENTATION_BANNER firmware_version=");
  Serial.print(config::kFirmwareVersion);
  Serial.print(" packet_version=");
  Serial.print(config::kImuPacketVersion);
  Serial.print(" packet_size=");
  Serial.print(sizeof(data::ImuPacket));
  Serial.print(" imu_sample_rate_hz=");
  Serial.print(config::kImuSampleRateHz);
  Serial.print(" bmp_sample_rate_hz=");
  Serial.print(config::kBmpSampleRateHz);
  Serial.print(" mag_sample_rate_hz=");
  Serial.print(config::kMagSampleRateHz);
  Serial.print(" mag_poll_rate_hz=");
  Serial.print(config::kMagPollRateHz);
  Serial.print(" accel_range_g=");
  Serial.print(config::kIcmAccelRangeG);
  Serial.print(" gyro_range_dps=");
  Serial.print(config::kIcmGyroRangeDps);
  Serial.print(" bmp_oversampling_enabled=");
  Serial.print(config::kBmpOversamplingEnabled ? 1 : 0);
  Serial.print(" bmp_pressure_oversampling=");
  Serial.print(config::kBmpOversamplingEnabled
                   ? config::kBmpPressureOversamplingFactor
                   : 1);
  Serial.print(" bmp_iir_filter_enabled=");
  Serial.print(config::kBmpIirFilterEnabled ? 1 : 0);
  Serial.print(" bmp_iir_coefficient=");
  Serial.print(config::kBmpIirFilterEnabled ? config::kBmpIirCoefficient : 0);
  Serial.print(" icm_dlpf_enabled=");
  Serial.print(config::kIcmDlpfEnabled ? 1 : 0);
  Serial.print(" icm_accel_dlpf_hz=");
  Serial.print(config::kIcmDlpfEnabled ? config::kIcmAccelDlpfLabel : "bypass");
  Serial.print(" icm_gyro_dlpf_hz=");
  Serial.print(config::kIcmDlpfEnabled ? config::kIcmGyroDlpfLabel : "bypass");
  Serial.print(" streaming=");
  Serial.println(streaming_ ? 1 : 0);
}

void PresentationStream::printIcmLine(size_t index) const {
  Serial.print("PRESENTATION_ICM index=");
  Serial.print(index);
  Serial.print(" channel=");
  Serial.print(icms_[index]->muxChannel());
  Serial.print(" ready=");
  Serial.print(icmReady_[index] ? 1 : 0);
  Serial.print(" address=0x");
  printHexByte(icms_[index]->address());
  Serial.print(" who_am_i=0x");
  printHexByte(icms_[index]->whoAmI());
  Serial.print(" mag_wia2=0x");
  printHexByte(icms_[index]->magnetometerWhoAmI());
  Serial.println();
}

void PresentationStream::printBmpLine(size_t index) const {
  Serial.print("PRESENTATION_BMP index=");
  Serial.print(index);
  Serial.print(" channel=");
  Serial.print(bmps_[index]->muxChannel());
  Serial.print(" ready=");
  Serial.print(bmpReady_[index] ? 1 : 0);
  Serial.print(" begin=");
  Serial.print(bmpBeginOk_[index] ? 1 : 0);
  Serial.print(" sampling=");
  Serial.print(bmpSamplingOk_[index] ? 1 : 0);
  Serial.print(" stage=");
  Serial.print(stageName(bmps_[index]->initStage()));
  Serial.print(" address=0x");
  printHexByte(bmps_[index]->address());
  Serial.print(" chip_id=0x");
  printHexByte(bmps_[index]->chipId());
  Serial.print(" last_chip_id=0x");
  printHexByte(bmps_[index]->lastChipId());
  Serial.println();
}

void PresentationStream::printBmpNvmLine(size_t index) const {
  Serial.print("PRESENTATION_BMP_NVM index=");
  Serial.print(index);
  Serial.print(" valid=");
  Serial.print(bmpNvmValid_[index] ? 1 : 0);
  Serial.print(" nvm=");
  if (bmpNvmValid_[index]) {
    for (size_t i = 0; i < drivers::kBmp390NvmLength; ++i) {
      printHexByte(bmpNvm_[index][i]);
    }
  }
  Serial.println();
}

}  // namespace pet::services
