#pragma once

#include <Audio.h>
#include <Arduino.h>

#include "config/constants.h"

namespace pet::services {

struct AudioPcmBlock {
  int16_t samples[config::kMicrophoneBlockSamples];
};

struct AudioCaptureCounters {
  uint32_t blocksReceived = 0;
  uint32_t blocksDropped = 0;
  uint32_t incompleteBlocks = 0;
  uint32_t samplesReceived = 0;
  uint16_t queueHighWaterBlocks = 0;
};

class AudioCaptureSink : public AudioStream {
 public:
  AudioCaptureSink();

  bool pop(AudioPcmBlock& block);
  AudioCaptureCounters counters() const;
  uint32_t firstSampleTimestampUs() const;
  virtual void update() override;

 private:
  audio_block_t* inputQueueArray_[1]{};
  AudioPcmBlock queue_[config::kMicrophoneQueueBlocks]{};
  volatile uint16_t head_ = 0;
  volatile uint16_t tail_ = 0;
  volatile uint16_t queueHighWaterBlocks_ = 0;
  volatile uint32_t blocksReceived_ = 0;
  volatile uint32_t blocksDropped_ = 0;
  volatile uint32_t incompleteBlocks_ = 0;
  volatile uint32_t firstSampleTimestampUs_ = 0;
};

class AudioCapture {
 public:
  bool begin();
  bool pop(AudioPcmBlock& block);
  AudioCaptureCounters counters() const;
  uint32_t firstSampleTimestampUs() const;
  bool started() const { return started_; }

 private:
  AudioCaptureSink sink_;
  AudioInputI2S* input_ = nullptr;
  AudioConnection* leftConnection_ = nullptr;
  bool started_ = false;
};

}  // namespace pet::services
