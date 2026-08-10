#pragma once

#include <Arduino.h>
#include <stddef.h>

namespace pet::data {

constexpr size_t kIcmCount = 3;
constexpr size_t kValuesPerIcm = 9;
constexpr size_t kImuValueCount = kIcmCount * kValuesPerIcm;

#pragma pack(push, 1)
struct ImuPacket {
  uint16_t magic;
  uint32_t timestampUs;
  uint16_t sequence;
  int16_t values[kImuValueCount];
  uint8_t crc8;
};
#pragma pack(pop)

static_assert(sizeof(ImuPacket) == 63, "Unexpected IMU packet size");
static_assert(offsetof(ImuPacket, crc8) == 62, "Unexpected CRC offset");

}  // namespace pet::data
