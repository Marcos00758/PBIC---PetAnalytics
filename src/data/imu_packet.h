#pragma once

#include <Arduino.h>
#include <stddef.h>

namespace pet::data {

constexpr size_t kIcmCount = 3;
constexpr size_t kValuesPerIcm = 9;
constexpr size_t kImuValueCount = kIcmCount * kValuesPerIcm;
constexpr size_t kBmpCount = 2;
constexpr uint32_t kInvalidBmpRaw = 0xFFFFFFFFUL;

struct BmpRawValues {
  uint32_t pressure;
  uint32_t temperature;
};

#pragma pack(push, 1)
struct ImuPacket {
  uint16_t magic;
  uint32_t timestampUs;
  uint16_t sequence;
  int16_t values[kImuValueCount];
  BmpRawValues bmp[kBmpCount];
  uint8_t crc8;
};
#pragma pack(pop)

static_assert(sizeof(BmpRawValues) == 8, "Unexpected BMP cache size");
static_assert(sizeof(ImuPacket) == 79, "Unexpected IMU packet size");
static_assert(offsetof(ImuPacket, bmp) == 62, "Unexpected BMP offset");
static_assert(offsetof(ImuPacket, crc8) == 78, "Unexpected CRC offset");

}  // namespace pet::data
