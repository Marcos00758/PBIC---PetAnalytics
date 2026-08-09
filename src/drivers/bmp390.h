#pragma once

#include <Adafruit_BMP3XX.h>
#include <Arduino.h>
#include <Wire.h>

#include "drivers/pca9548a.h"

namespace pet::drivers {

struct Bmp390RawSample {
  uint32_t pressure;
  uint32_t temperature;
};

class Bmp390 {
 public:
  Bmp390(Pca9548a& mux, TwoWire& wire, uint8_t muxChannel);

  bool begin();
  bool startRawSampling25Hz();
  bool readRaw(Bmp390RawSample& sample);

  bool initialized() const { return initialized_; }
  uint8_t muxChannel() const { return muxChannel_; }
  uint8_t address() const { return address_; }
  uint8_t chipId() const { return chipId_; }

 private:
  bool addressResponds(uint8_t address);
  bool writeRegister(uint8_t reg, uint8_t value);
  bool readRegisters(uint8_t startRegister, uint8_t* data, size_t length);

  Pca9548a& mux_;
  TwoWire& wire_;
  const uint8_t muxChannel_;
  Adafruit_BMP3XX sensor_;
  uint8_t address_ = 0;
  uint8_t chipId_ = 0;
  bool initialized_ = false;
};

}  // namespace pet::drivers
