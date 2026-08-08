#pragma once

#include <Arduino.h>
#include <Wire.h>

namespace pet::drivers {

class Pca9548a {
 public:
  Pca9548a(TwoWire& wire, uint8_t address, uint16_t settleTimeUs);

  bool begin();
  bool isConnected();
  bool selectChannel(uint8_t channel);
  bool disableAllChannels();

  uint8_t address() const { return address_; }
  int8_t selectedChannel() const { return selectedChannel_; }

 private:
  bool writeControl(uint8_t controlByte);

  TwoWire& wire_;
  const uint8_t address_;
  const uint16_t settleTimeUs_;
  int8_t selectedChannel_ = -1;
};

}  // namespace pet::drivers
