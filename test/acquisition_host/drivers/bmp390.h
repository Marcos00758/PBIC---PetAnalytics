#pragma once
#include <Arduino.h>
namespace pet::drivers {
struct Bmp390RawSample { uint32_t pressure, temperature; };
class Bmp390 {
 public:
  uint32_t pressure = 0x010203, temperature = 0x040506;
  bool readRaw(Bmp390RawSample& sample) {
    hostNowUs += 30;
    sample = {pressure, temperature};
    return true;
  }
};
}  // namespace pet::drivers
