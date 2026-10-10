#pragma once
#include <Arduino.h>
namespace pet::drivers {
struct Vector3i16 { int16_t x, y, z; };
struct Icm20948RawSample { Vector3i16 acceleration, gyro; };
struct Ak09916RawSample {
  Vector3i16 magnetic;
  bool dataReady = true, dataOverrun = false, overflow = false;
};
class Icm20948 {
 public:
  int16_t base = 100;
  bool readable = true;
  bool readRaw(Icm20948RawSample& sample) {
    hostNowUs += 50;
    if (!readable) return false;
    sample.acceleration = {base, int16_t(-base), int16_t(base + 1)};
    sample.gyro = {int16_t(base + 2), int16_t(base + 3), int16_t(base + 4)};
    return true;
  }
  bool readMagnetometerRaw(Ak09916RawSample& sample) {
    hostNowUs += 20;
    sample.magnetic = {int16_t(base + 5), int16_t(base + 6), int16_t(base + 7)};
    return true;
  }
};
}  // namespace pet::drivers
