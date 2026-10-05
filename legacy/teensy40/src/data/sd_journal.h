#pragma once

#include <Arduino.h>
#include <stddef.h>

namespace pet::data {

constexpr uint16_t kSdJournalMagic = 0x4A50;
constexpr uint8_t kSdJournalVersion = 1;

enum class SdJournalState : uint8_t {
  kRecording = 1,
  kStopped = 2,
  kCompleted = 3,
};

#pragma pack(push, 1)
struct SdJournalRecord {
  uint16_t magic;
  uint8_t version;
  SdJournalState state;
  uint32_t sequence;
  uint32_t uptimeMs;
  uint64_t imuValidBytes;
  uint64_t audioValidBytes;
  uint8_t reserved[3];
  uint8_t crc8;
};
#pragma pack(pop)

static_assert(sizeof(SdJournalRecord) == 32,
              "Unexpected SD journal record size");
static_assert(offsetof(SdJournalRecord, crc8) == 31,
              "Unexpected SD journal CRC offset");

}  // namespace pet::data
