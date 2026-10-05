#pragma once

#include <Adafruit_BMP3XX.h>
#include <Arduino.h>
#include <Wire.h>

#include "drivers/pca9548a.h"

namespace pet::drivers {

constexpr size_t kBmp390NvmLength = 21;

struct Bmp390RawSample {
  uint32_t pressure;
  uint32_t temperature;
};

// How far begin() got before giving up. Ordered so the furthest stage reached
// across both candidate addresses is the one worth reporting.
enum class Bmp390InitStage : uint8_t {
  kNotAttempted = 0,
  kChannelSelect = 1,
  kAddressProbe = 2,
  kLibraryBegin = 3,
  kChipId = 4,
  kReady = 5,
};

class Bmp390 {
 public:
  Bmp390(Pca9548a& mux, TwoWire& wire, uint8_t muxChannel);

  bool begin();
  bool startRawSampling25Hz();
  bool readRaw(Bmp390RawSample& sample);
  bool readNvm(uint8_t (&data)[kBmp390NvmLength]);

  bool initialized() const { return initialized_; }
  uint8_t muxChannel() const { return muxChannel_; }
  uint8_t address() const { return address_; }
  uint8_t chipId() const { return chipId_; }
  Bmp390InitStage initStage() const { return stage_; }
  uint8_t lastChipId() const { return lastChipId_; }

 private:
  bool addressResponds(uint8_t address);
  void recordStage(Bmp390InitStage stage);
  bool writeRegister(uint8_t reg, uint8_t value);
  bool readRegisters(uint8_t startRegister, uint8_t* data, size_t length);

  Pca9548a& mux_;
  TwoWire& wire_;
  const uint8_t muxChannel_;
  Adafruit_BMP3XX sensor_;
  uint8_t address_ = 0;
  uint8_t chipId_ = 0;
  uint8_t lastChipId_ = 0;
  Bmp390InitStage stage_ = Bmp390InitStage::kNotAttempted;
  bool initialized_ = false;
};

}  // namespace pet::drivers
