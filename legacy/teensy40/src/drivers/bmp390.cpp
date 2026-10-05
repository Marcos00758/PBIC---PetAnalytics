#include "drivers/bmp390.h"

#include "config/constants.h"

namespace pet::drivers {
namespace {

constexpr uint8_t kDataRegister = 0x04;
constexpr uint8_t kNvmStartRegister = 0x31;
constexpr uint8_t kOversamplingRegister = 0x1C;
constexpr uint8_t kOutputDataRateRegister = 0x1D;
constexpr uint8_t kPowerControlRegister = 0x1B;
constexpr uint8_t kConfigRegister = 0x1F;
constexpr uint8_t kNoOversampling = 0x00;
constexpr uint8_t kIirFilterBypass = 0x00;
constexpr uint8_t kTemperatureOversamplingShift = 3;
constexpr uint8_t kIirCoefficientShift = 1;
constexpr uint8_t kOutputDataRate25Hz = 0x03;
constexpr uint8_t kPressureTemperatureNormalMode = 0x33;

uint32_t decodeLittleEndianUint24(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16);
}

}  // namespace

Bmp390::Bmp390(Pca9548a& mux, TwoWire& wire, uint8_t muxChannel)
    : mux_(mux), wire_(wire), muxChannel_(muxChannel) {}

bool Bmp390::begin() {
  initialized_ = false;
  address_ = 0;
  chipId_ = 0;
  lastChipId_ = 0;
  stage_ = Bmp390InitStage::kNotAttempted;

  recordStage(Bmp390InitStage::kChannelSelect);
  if (!mux_.selectChannel(muxChannel_)) {
    return false;
  }

  for (const uint8_t candidate : config::kBmpCandidateAddresses) {
    recordStage(Bmp390InitStage::kAddressProbe);
    if (!addressResponds(candidate)) {
      continue;
    }
    recordStage(Bmp390InitStage::kLibraryBegin);
    if (!mux_.selectChannel(muxChannel_) ||
        !sensor_.begin_I2C(candidate, &wire_)) {
      continue;
    }

    recordStage(Bmp390InitStage::kChipId);
    const uint8_t detectedChipId = sensor_.chipID();
    lastChipId_ = detectedChipId;
    if (detectedChipId != config::kBmpExpectedChipId) {
      continue;
    }

    address_ = candidate;
    chipId_ = detectedChipId;
    initialized_ = true;
    recordStage(Bmp390InitStage::kReady);
    return true;
  }

  return false;
}

// Keeps the furthest stage reached. Without this the second candidate address,
// which normally does not answer, would erase how far the first one got.
void Bmp390::recordStage(Bmp390InitStage stage) {
  if (static_cast<uint8_t>(stage) > static_cast<uint8_t>(stage_)) {
    stage_ = stage;
  }
}

bool Bmp390::startRawSampling25Hz() {
  if (!initialized_ || !mux_.selectChannel(muxChannel_)) {
    return false;
  }

  const uint8_t oversampling =
      config::kBmpOversamplingEnabled
          ? static_cast<uint8_t>(
                (config::kBmpTemperatureOversamplingCode
                 << kTemperatureOversamplingShift) |
                config::kBmpPressureOversamplingCode)
          : kNoOversampling;
  const uint8_t iirFilter =
      config::kBmpIirFilterEnabled
          ? static_cast<uint8_t>(config::kBmpIirCoefficientCode
                                 << kIirCoefficientShift)
          : kIirFilterBypass;

  // Power control comes last so the sensor only enters normal mode once the
  // oversampling, filter and rate registers already hold the final values.
  return writeRegister(kOversamplingRegister, oversampling) &&
         writeRegister(kConfigRegister, iirFilter) &&
         writeRegister(kOutputDataRateRegister, kOutputDataRate25Hz) &&
         writeRegister(kPowerControlRegister,
                       kPressureTemperatureNormalMode);
}

bool Bmp390::readRaw(Bmp390RawSample& sample) {
  if (!initialized_ || !mux_.selectChannel(muxChannel_)) {
    return false;
  }

  uint8_t data[6]{};
  if (!readRegisters(kDataRegister, data, sizeof(data))) {
    return false;
  }
  sample.pressure = decodeLittleEndianUint24(&data[0]);
  sample.temperature = decodeLittleEndianUint24(&data[3]);
  return true;
}

bool Bmp390::readNvm(uint8_t (&data)[kBmp390NvmLength]) {
  if (!initialized_ || !mux_.selectChannel(muxChannel_)) {
    return false;
  }
  return readRegisters(kNvmStartRegister, data, sizeof(data));
}

bool Bmp390::addressResponds(uint8_t address) {
  wire_.beginTransmission(address);
  return wire_.endTransmission() == 0;
}

bool Bmp390::writeRegister(uint8_t reg, uint8_t value) {
  wire_.beginTransmission(address_);
  wire_.write(reg);
  wire_.write(value);
  return wire_.endTransmission() == 0;
}

bool Bmp390::readRegisters(uint8_t startRegister, uint8_t* data,
                           size_t length) {
  if (data == nullptr || length == 0 || length > 255) {
    return false;
  }
  wire_.beginTransmission(address_);
  wire_.write(startRegister);
  if (wire_.endTransmission(false) != 0) {
    return false;
  }
  const uint8_t requested = static_cast<uint8_t>(length);
  if (wire_.requestFrom(address_, requested) != requested) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (!wire_.available()) {
      return false;
    }
    data[i] = static_cast<uint8_t>(wire_.read());
  }
  return true;
}

}  // namespace pet::drivers
