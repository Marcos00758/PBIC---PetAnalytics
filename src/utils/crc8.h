#pragma once

#include <Arduino.h>
#include <stddef.h>

namespace pet::utils {

uint8_t crc8(const uint8_t* data, size_t length);

}  // namespace pet::utils
