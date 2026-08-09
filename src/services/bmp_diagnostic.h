#pragma once

#include <Arduino.h>

#include "drivers/bmp390.h"

namespace pet::services {

struct ScalarStatistics {
  uint32_t count = 0;
  double mean = 0.0;
  double minimum = 0.0;
  double maximum = 0.0;
  double standardDeviation = 0.0;
};

struct BmpDiagnosticSensorResult {
  ScalarStatistics temperatureC;
  ScalarStatistics pressurePa;
  uint32_t readFailures = 0;
};

struct BmpDiagnosticResult {
  BmpDiagnosticSensorResult sensors[2];
  uint32_t elapsedMs = 0;
};

BmpDiagnosticResult runBmpDiagnostic(drivers::Bmp390& bmp0,
                                     drivers::Bmp390& bmp1,
                                     uint16_t rounds,
                                     uint16_t warmupRounds,
                                     uint32_t periodUs);

}  // namespace pet::services
