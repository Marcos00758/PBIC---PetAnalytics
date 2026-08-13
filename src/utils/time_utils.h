#pragma once

#include <Arduino.h>

namespace pet::utils {

constexpr uint32_t elapsedMicros(uint32_t startUs, uint32_t endUs) {
  return endUs - startUs;
}

constexpr bool deadlineReached(uint32_t nowUs, uint32_t deadlineUs) {
  return static_cast<int32_t>(nowUs - deadlineUs) >= 0;
}

static_assert(elapsedMicros(0xFFFFFF00U, 0x00000100U) == 0x200U,
              "micros rollover elapsed-time calculation is invalid");
static_assert(!deadlineReached(0xFFFFFF00U, 0x00000100U),
              "deadline must remain pending across micros rollover");
static_assert(deadlineReached(0x00000100U, 0xFFFFFF00U),
              "deadline must expire across micros rollover");

}  // namespace pet::utils
