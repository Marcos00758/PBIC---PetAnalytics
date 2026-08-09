#include "drivers/bmp390.h"

#include <math.h>

#include "config/constants.h"

namespace pet::drivers {

Bmp390::Bmp390(Pca9548a& mux, TwoWire& wire, uint8_t muxChannel)
    : mux_(mux), wire_(wire), muxChannel_(muxChannel) {}

bool Bmp390::begin() {
  initialized_ = false;
  address_ = 0;
  chipId_ = 0;

  if (!mux_.selectChannel(muxChannel_)) {
    return false;
  }

  for (const uint8_t candidate : config::kBmpCandidateAddresses) {
    if (!addressResponds(candidate)) {
      continue;
    }
    if (!mux_.selectChannel(muxChannel_) ||
        !sensor_.begin_I2C(candidate, &wire_)) {
      continue;
    }

    const uint8_t detectedChipId = sensor_.chipID();
    if (detectedChipId != config::kBmpExpectedChipId) {
      continue;
    }

    address_ = candidate;
    chipId_ = detectedChipId;
    initialized_ = true;
    return true;
  }

  return false;
}

bool Bmp390::read(Bmp390Sample& sample) {
  if (!initialized_ || !mux_.selectChannel(muxChannel_) ||
      !sensor_.performReading()) {
    return false;
  }

  sample.temperatureC = static_cast<float>(sensor_.temperature);
  sample.pressurePa = static_cast<float>(sensor_.pressure);
  return isfinite(sample.temperatureC) && isfinite(sample.pressurePa) &&
         sample.pressurePa > 0.0f;
}

bool Bmp390::addressResponds(uint8_t address) {
  wire_.beginTransmission(address);
  return wire_.endTransmission() == 0;
}

}  // namespace pet::drivers
