#pragma once

#include <Arduino.h>

#include "config/constants.h"

// Historical PCM and metadata types only. No Audio.h, I2S or capture allocation.
// Retained so SD metadata/journal and offline readers remain compatible.
namespace pet::services {

struct AudioPcmBlock {
  uint32_t timestampUs = 0;
  uint32_t sequence = 0;
  int16_t samples[config::kMicrophoneBlockSamples];
};

constexpr size_t kAudioPcmBytesPerBlock =
    config::kMicrophoneBlockSamples * sizeof(int16_t);

struct AudioCaptureCounters {
  uint32_t blocksReceived = 0;
  uint32_t blocksDropped = 0;
  uint32_t incompleteBlocks = 0;
  uint32_t samplesReceived = 0;
  uint16_t queueHighWaterBlocks = 0;
};

struct AudioPreflightResult {
  bool valid = false;
  bool accepted = false;
  uint32_t samples = 0;
  int32_t meanCounts = 0;
  uint32_t rmsCounts = 0;
  uint16_t peakCounts = 0;
  uint32_t clippingSamples = 0;
};

}  // namespace pet::services
