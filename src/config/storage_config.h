#pragma once
#include <stddef.h>
#include <stdint.h>

namespace pet::storage {
// Defaults shared by the logger and the independent SD diagnostic.
constexpr size_t kBufferBytes = 8192;
constexpr size_t kWriteBlockBytes = 512;
constexpr uint8_t kSpiClockMHz = 12;
constexpr uint64_t kFreeReserveBytes = 4ULL * 1024 * 1024;
}  // namespace pet::storage
