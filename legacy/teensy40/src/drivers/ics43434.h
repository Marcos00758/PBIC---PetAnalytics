#pragma once

#include <Audio.h>
#include <Arduino.h>

namespace pet::drivers {

struct StereoAudioBlock {
  int16_t left[AUDIO_BLOCK_SAMPLES];
  int16_t right[AUDIO_BLOCK_SAMPLES];
};

class AudioRamSink : public AudioStream {
 public:
  AudioRamSink();

  bool pop(StereoAudioBlock& block);
  uint32_t framesSeen() const;
  uint32_t framesDropped() const;
  uint32_t incompleteFrames() const;
  uint8_t queueHighWater() const;
  virtual void update() override;

 private:
  static constexpr uint8_t kQueueDepth = 16;
  audio_block_t* inputQueueArray_[2]{};
  StereoAudioBlock queue_[kQueueDepth]{};
  volatile uint8_t head_ = 0;
  volatile uint8_t tail_ = 0;
  volatile uint8_t queueHighWater_ = 0;
  volatile uint32_t framesSeen_ = 0;
  volatile uint32_t framesDropped_ = 0;
  volatile uint32_t incompleteFrames_ = 0;
};

class Ics43434Diagnostic {
 public:
  bool begin();
  void poll();

 private:
  struct ChannelStatistics {
    uint64_t samples = 0;
    int64_t sum = 0;
    uint64_t sumSquares = 0;
    uint64_t lsbOnes = 0;
    uint32_t clippingSamples = 0;
    uint32_t maxAbsolute = 0;
    int16_t minimum = INT16_MAX;
    int16_t maximum = INT16_MIN;
  };

  void accumulate(ChannelStatistics& statistics, const int16_t* samples,
                  size_t count);
  void printReport();
  void printChannel(const char* name,
                    const ChannelStatistics& statistics) const;
  double acRms(const ChannelStatistics& statistics) const;
  uint8_t signedBitsUsed(uint32_t maximumAbsolute) const;

  AudioRamSink sink_;
  AudioInputI2S* input_ = nullptr;
  AudioConnection* leftConnection_ = nullptr;
  AudioConnection* rightConnection_ = nullptr;
  ChannelStatistics left_{};
  ChannelStatistics right_{};
  uint32_t startedUs_ = 0;
  uint32_t initialFramesSeen_ = 0;
  uint32_t nextReportMs_ = 0;
  bool started_ = false;
};

}  // namespace pet::drivers
