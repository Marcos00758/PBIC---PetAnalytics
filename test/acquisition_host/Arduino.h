#pragma once
#include <stddef.h>
#include <stdint.h>
extern uint32_t hostNowUs;
inline uint32_t micros() { return hostNowUs; }
