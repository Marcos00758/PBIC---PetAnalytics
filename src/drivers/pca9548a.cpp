#include "drivers/pca9548a.h"

namespace pet::drivers {

Pca9548a::Pca9548a(TwoWire& wire, uint8_t address, uint16_t settleTimeUs)
    : wire_(wire), address_(address), settleTimeUs_(settleTimeUs) {}

bool Pca9548a::begin() {
  if (!isConnected()) {
    return false;
  }
  return disableAllChannels();
}

bool Pca9548a::isConnected() {
  wire_.beginTransmission(address_);
  return wire_.endTransmission() == 0;
}

bool Pca9548a::selectChannel(uint8_t channel) {
  if (channel > 7) {
    return false;
  }

  if (!writeControl(static_cast<uint8_t>(1U << channel))) {
    selectedChannel_ = -1;
    return false;
  }

  selectedChannel_ = static_cast<int8_t>(channel);
  delayMicroseconds(settleTimeUs_);
  return true;
}

bool Pca9548a::disableAllChannels() {
  if (!writeControl(0x00)) {
    return false;
  }
  selectedChannel_ = -1;
  return true;
}

bool Pca9548a::writeControl(uint8_t controlByte) {
  wire_.beginTransmission(address_);
  wire_.write(controlByte);
  return wire_.endTransmission() == 0;
}

}  // namespace pet::drivers
