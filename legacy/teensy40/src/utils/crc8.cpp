#include "utils/crc8.h"

#include "config/constants.h"

namespace pet::utils {

uint8_t crc8(const uint8_t* data, size_t length) {
  uint8_t crc = 0;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80U)
                ? static_cast<uint8_t>((crc << 1U) ^ config::kCrc8Polynomial)
                : static_cast<uint8_t>(crc << 1U);
    }
  }
  return crc;
}

}  // namespace pet::utils
