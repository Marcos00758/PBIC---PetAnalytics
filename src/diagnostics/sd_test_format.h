#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace pet::sdtest {
constexpr size_t kRecordBytes = 256;
constexpr size_t kCheckpointBytes = 32;
constexpr uint32_t kDataMagic = 0x31544453;  // SDT1, little endian
constexpr uint32_t kCheckpointMagic = 0x31434453;  // SDC1
inline uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t crc = 0xffffffffU;
  while (n--) {
    crc ^= *p++;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return crc ^ 0xffffffffU;
}
inline void put32(uint8_t* p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = v >> (8 * i);
}
inline uint32_t get32(const uint8_t* p) {
  uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i) v |= uint32_t(p[i]) << (8 * i);
  return v;
}
inline void put64(uint8_t* p, uint64_t v) {
  put32(p, uint32_t(v)); put32(p + 4, uint32_t(v >> 32));
}
inline uint64_t get64(const uint8_t* p) {
  return get32(p) | (uint64_t(get32(p + 4)) << 32);
}
inline uint32_t mix(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU;
  x ^= x >> 15; x *= 0x846ca68bU;
  return x ^ (x >> 16);
}
inline void record(uint8_t* p, uint64_t sequence, uint32_t seed) {
  put32(p, kDataMagic); put32(p + 4, seed); put64(p + 8, sequence);
  for (unsigned i = 16; i < 252; ++i)
    p[i] = uint8_t(mix(seed ^ uint32_t(sequence) ^
                     (uint32_t(sequence >> 32) * 0x9e3779b9U) ^
                     (i * 0x85ebca6bU)));
  put32(p + 252, crc32(p, 252));
}
inline void fill(uint8_t* out, size_t n, uint64_t offset, uint32_t seed) {
  uint8_t temp[kRecordBytes];
  while (n) {
    record(temp, offset / kRecordBytes, seed);
    const size_t start = offset % kRecordBytes;
    const size_t count = n < kRecordBytes - start ? n : kRecordBytes - start;
    memcpy(out, temp + start, count);
    out += count; offset += count; n -= count;
  }
}
inline void checkpoint(uint8_t* p, uint32_t seed, uint64_t durable,
                       uint64_t planned, bool complete) {
  put32(p, kCheckpointMagic); put32(p + 4, seed);
  put64(p + 8, durable); put64(p + 16, planned);
  put32(p + 24, complete ? 1 : 0); put32(p + 28, crc32(p, 28));
}
inline bool validCheckpoint(const uint8_t* p) {
  const uint64_t durable = get64(p + 8), planned = get64(p + 16);
  const uint32_t state = get32(p + 24);
  return get32(p) == kCheckpointMagic && get32(p + 28) == crc32(p, 28) &&
         planned > 0 && planned % kRecordBytes == 0 &&
         durable <= planned && durable % kRecordBytes == 0 && state <= 1 &&
         (state != 1 || durable == planned);
}
}  // namespace pet::sdtest
