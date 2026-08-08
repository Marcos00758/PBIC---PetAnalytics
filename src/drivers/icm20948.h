#pragma once

#include <Adafruit_ICM20948.h>
#include <Adafruit_Sensor.h>
#include <Arduino.h>
#include <Wire.h>

#include "drivers/pca9548a.h"

namespace pet::drivers {

struct Vector3f {
  float x;
  float y;
  float z;
};

struct Vector3i16 {
  int16_t x;
  int16_t y;
  int16_t z;
};

struct Icm20948RawSample {
  Vector3i16 acceleration;
  Vector3i16 gyro;
};

struct Icm20948Sample {
  uint32_t timestampUs;
  Vector3f accelerationMps2;
  Vector3f gyroDps;
};

class Icm20948 {
 public:
  Icm20948(Pca9548a& mux, TwoWire& wire, uint8_t muxChannel,
           int32_t sensorId);

  bool begin();
  bool read(Icm20948Sample& sample);
  bool readRaw(Icm20948RawSample& sample);

  bool initialized() const { return initialized_; }
  uint8_t muxChannel() const { return muxChannel_; }
  uint8_t address() const { return address_; }
  uint8_t whoAmI() const { return whoAmI_; }

 private:
  bool detectAddress();
  bool readWhoAmI(uint8_t address, uint8_t& value);
  bool writeRegister(uint8_t address, uint8_t reg, uint8_t value);
  bool readRegister(uint8_t address, uint8_t reg, uint8_t& value);
  bool readRegisters(uint8_t address, uint8_t startRegister, uint8_t* data,
                     size_t length);
  bool configure();

  Pca9548a& mux_;
  TwoWire& wire_;
  const uint8_t muxChannel_;
  const int32_t sensorId_;
  Adafruit_ICM20948 sensor_;
  uint8_t address_ = 0;
  uint8_t whoAmI_ = 0;
  bool initialized_ = false;
};

}  // namespace pet::drivers
