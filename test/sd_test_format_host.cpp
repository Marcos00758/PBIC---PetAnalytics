// Host fixture writer: Python verifies bytes produced by the actual C++ format.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/diagnostics/sd_test_format.h"
namespace fmt = pet::sdtest;
int main(int argc, char** argv) {
  assert(argc == 2);
  assert(fmt::crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xcbf43926);
  uint8_t bytes[512], chunks[512], check[32];
  constexpr uint32_t seed = 0x50424943;
  fmt::fill(bytes, 512, 0, seed);
  for (size_t offset = 0; offset < 512;) {
    size_t n = 512 - offset < 79 ? 512 - offset : 79;
    fmt::fill(chunks + offset, n, offset, seed); offset += n;
  }
  assert(memcmp(bytes, chunks, 512) == 0);
  uint8_t high[256]; fmt::record(high, 0x100000001ULL, seed);
  assert(fmt::get64(high + 8) == 0x100000001ULL);
  assert(memcmp(high, bytes + 256, 256) != 0);
  fmt::checkpoint(check, seed, 512, 512, true);
  assert(fmt::validCheckpoint(check));
  check[8] ^= 1; assert(!fmt::validCheckpoint(check)); check[8] ^= 1;
  char name[1024];
  snprintf(name, sizeof(name), "%s/c000.bin", argv[1]);
  FILE* out = fopen(name, "wb"); assert(out);
  assert(fwrite(bytes, 1, 512, out) == 512); assert(fclose(out) == 0);
  snprintf(name, sizeof(name), "%s/c000.chk", argv[1]);
  out = fopen(name, "wb"); assert(out);
  assert(fwrite(check, 1, 32, out) == 32); assert(fclose(out) == 0);
}
