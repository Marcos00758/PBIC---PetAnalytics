#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pet::config {
constexpr uint8_t kAcquisitionCore = 1;
constexpr uint8_t kStorageCore = 0;
constexpr uint32_t kAcquisitionPriority = 3;
constexpr uint32_t kStoragePriority = 2;
constexpr uint32_t kAcquisitionStackBytes = 6144;
constexpr uint32_t kStorageStackBytes = 8192;
constexpr size_t kPacketQueueCapacity = 256;  // 2.56 s at nominal 100 Hz.
constexpr uint32_t kRuntimeReportMs = 5000;
constexpr uint32_t kI2cTimeoutMs = 10;
}  // namespace pet::config
