#include "services/bmp_diagnostic.h"

#include <math.h>

namespace pet::services {
namespace {

class RunningStatistics {
 public:
  void add(double value) {
    ++count_;
    const double delta = value - mean_;
    mean_ += delta / count_;
    const double deltaAfterMean = value - mean_;
    sumSquaredDifferences_ += delta * deltaAfterMean;

    if (count_ == 1 || value < minimum_) {
      minimum_ = value;
    }
    if (count_ == 1 || value > maximum_) {
      maximum_ = value;
    }
  }

  ScalarStatistics result() const {
    ScalarStatistics output{};
    output.count = count_;
    if (count_ == 0) {
      return output;
    }
    output.mean = mean_;
    output.minimum = minimum_;
    output.maximum = maximum_;
    output.standardDeviation = sqrt(sumSquaredDifferences_ / count_);
    return output;
  }

 private:
  uint32_t count_ = 0;
  double mean_ = 0.0;
  double minimum_ = 0.0;
  double maximum_ = 0.0;
  double sumSquaredDifferences_ = 0.0;
};

}  // namespace

BmpDiagnosticResult runBmpDiagnostic(drivers::Bmp390& bmp0,
                                     drivers::Bmp390& bmp1,
                                     uint16_t rounds,
                                     uint16_t warmupRounds,
                                     uint32_t periodUs) {
  BmpDiagnosticResult result{};
  drivers::Bmp390* const sensors[] = {&bmp0, &bmp1};
  RunningStatistics temperatures[2];
  RunningStatistics pressures[2];
  const uint32_t startMs = millis();
  uint32_t nextRoundUs = micros();

  for (uint16_t round = 0; round < rounds; ++round) {
    while (static_cast<int32_t>(micros() - nextRoundUs) < 0) {
      yield();
    }

    for (size_t i = 0; i < 2; ++i) {
      if (!sensors[i]->initialized()) {
        continue;
      }

      drivers::Bmp390Sample sample{};
      if (!sensors[i]->read(sample)) {
        ++result.sensors[i].readFailures;
        continue;
      }

      if (round >= warmupRounds) {
        temperatures[i].add(sample.temperatureC);
        pressures[i].add(sample.pressurePa);
      }
    }

    nextRoundUs += periodUs;
    const uint32_t nowUs = micros();
    if (static_cast<int32_t>(nowUs - nextRoundUs) >= 0) {
      nextRoundUs = nowUs + periodUs;
    }
  }

  for (size_t i = 0; i < 2; ++i) {
    result.sensors[i].temperatureC = temperatures[i].result();
    result.sensors[i].pressurePa = pressures[i].result();
  }
  result.elapsedMs = millis() - startMs;
  return result;
}

}  // namespace pet::services
