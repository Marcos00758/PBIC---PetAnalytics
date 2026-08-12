#include "services/audio_capture.h"

#include <math.h>
#include <string.h>

#include "config/pins.h"

namespace pet::services {
namespace {

constexpr uint32_t kAudioBlockDurationUs =
    (static_cast<uint64_t>(config::kMicrophoneBlockSamples) * 1000000ULL +
     config::kMicrophoneSampleRateHz / 2U) /
    config::kMicrophoneSampleRateHz;

uint16_t queueCount(uint16_t head, uint16_t tail) {
  return head >= tail
             ? head - tail
             : config::kMicrophoneQueueBlocks + head - tail;
}

}  // namespace

static_assert(AUDIO_BLOCK_SAMPLES == config::kMicrophoneBlockSamples,
              "Teensy Audio block size changed");
static_assert(static_cast<uint32_t>(AUDIO_SAMPLE_RATE_EXACT) ==
                  config::kMicrophoneSampleRateHz,
              "Teensy Audio sample rate changed");

AudioCaptureSink::AudioCaptureSink() : AudioStream(1, inputQueueArray_) {}

bool AudioCaptureSink::pop(AudioPcmBlock& block) {
  const uint16_t tail = tail_;
  if (tail == head_) {
    return false;
  }
  block.timestampUs = queue_[tail].timestampUs;
  block.sequence = queue_[tail].sequence;
  memcpy(block.samples, queue_[tail].samples, sizeof(block.samples));
  __disable_irq();
  tail_ = static_cast<uint16_t>((tail + 1U) % config::kMicrophoneQueueBlocks);
  __enable_irq();
  return true;
}

AudioCaptureCounters AudioCaptureSink::counters() const {
  AudioCaptureCounters snapshot{};
  __disable_irq();
  snapshot.blocksReceived = blocksReceived_;
  snapshot.blocksDropped = blocksDropped_;
  snapshot.incompleteBlocks = incompleteBlocks_;
  snapshot.queueHighWaterBlocks = queueHighWaterBlocks_;
  __enable_irq();
  snapshot.samplesReceived =
      snapshot.blocksReceived * config::kMicrophoneBlockSamples;
  return snapshot;
}

uint32_t AudioCaptureSink::firstSampleTimestampUs() const {
  __disable_irq();
  const uint32_t timestamp = firstSampleTimestampUs_;
  __enable_irq();
  return timestamp;
}

void AudioCaptureSink::prepareForRecording() {
  __disable_irq();
  tail_ = head_;
  firstSampleTimestampUs_ = 0;
  queueHighWaterBlocks_ = 0;
  blocksReceived_ = 0;
  blocksDropped_ = 0;
  incompleteBlocks_ = 0;
  blockSequence_ = 0;
  __enable_irq();
}

void AudioCaptureSink::update() {
  const uint32_t sequence = blockSequence_++;
  audio_block_t* left = receiveReadOnly(0);
  if (left == nullptr) {
    ++incompleteBlocks_;
    return;
  }

  if (firstSampleTimestampUs_ == 0) {
    firstSampleTimestampUs_ = micros() - kAudioBlockDurationUs;
  }

  ++blocksReceived_;
  const uint16_t nextHead = static_cast<uint16_t>(
      (head_ + 1U) % config::kMicrophoneQueueBlocks);
  if (nextHead == tail_) {
    ++blocksDropped_;
  } else {
    queue_[head_].timestampUs = micros() - kAudioBlockDurationUs;
    queue_[head_].sequence = sequence;
    memcpy(queue_[head_].samples, left->data, sizeof(queue_[head_].samples));
    head_ = nextHead;
    const uint16_t count = queueCount(head_, tail_);
    if (count > queueHighWaterBlocks_) {
      queueHighWaterBlocks_ = count;
    }
  }
  release(left);
}

bool AudioCapture::begin() {
  static_assert(pins::kMicBclk == 21,
                "Teensy 4.x AudioInputI2S BCLK is pin 21");
  static_assert(pins::kMicLrclk == 20,
                "Teensy 4.x AudioInputI2S LRCLK is pin 20");
  static_assert(pins::kMicData == 8,
                "Teensy 4.x AudioInputI2S RX is pin 8");

  AudioMemory(config::kMicrophoneAudioMemoryBlocks);
  static AudioInputI2S input;
  static AudioConnection leftConnection(input, 0, sink_, 0);
  input_ = &input;
  leftConnection_ = &leftConnection;
  started_ = true;
  return true;
}

AudioPreflightResult AudioCapture::runQuietPreflight(uint32_t durationMs) {
  AudioPreflightResult result{};
  if (!started_) {
    return result;
  }

  int64_t sum = 0;
  uint64_t sumSquares = 0;
  uint32_t peak = 0;
  const uint32_t startedMs = millis();
  AudioPcmBlock block{};
  while (millis() - startedMs < durationMs) {
    if (!pop(block)) {
      yield();
      continue;
    }
    for (int16_t sample : block.samples) {
      const int32_t value = sample;
      const uint32_t absolute =
          value < 0 ? static_cast<uint32_t>(-value)
                    : static_cast<uint32_t>(value);
      sum += value;
      sumSquares += static_cast<uint64_t>(value * value);
      if (absolute > peak) {
        peak = absolute;
      }
      if (absolute >= 32760U) {
        ++result.clippingSamples;
      }
      ++result.samples;
    }
  }

  if (result.samples == 0) {
    return result;
  }
  result.valid = true;
  result.meanCounts = static_cast<int32_t>(sum / result.samples);
  const double mean = static_cast<double>(sum) / result.samples;
  double variance = static_cast<double>(sumSquares) / result.samples -
                    mean * mean;
  if (variance < 0.0) {
    variance = 0.0;
  }
  result.rmsCounts = static_cast<uint32_t>(sqrt(variance) + 0.5);
  result.peakCounts = static_cast<uint16_t>(peak > 32767U ? 32767U : peak);
  const uint64_t clippingPpm =
      static_cast<uint64_t>(result.clippingSamples) * 1000000ULL /
      result.samples;
  const int32_t absoluteMean =
      result.meanCounts < 0 ? -result.meanCounts : result.meanCounts;
  result.accepted =
      absoluteMean <= config::kAudioPreflightMaximumAbsMeanCounts &&
      result.rmsCounts <= config::kAudioPreflightMaximumRmsCounts &&
      clippingPpm <= config::kAudioPreflightMaximumClippingPpm;
  return result;
}

void AudioCapture::prepareForRecording() {
  if (started_) {
    sink_.prepareForRecording();
  }
}

void AudioCapture::disable() {
  if (leftConnection_ != nullptr) {
    leftConnection_->disconnect();
  }
  started_ = false;
}

bool AudioCapture::pop(AudioPcmBlock& block) {
  return started_ && sink_.pop(block);
}

AudioCaptureCounters AudioCapture::counters() const {
  return sink_.counters();
}

uint32_t AudioCapture::firstSampleTimestampUs() const {
  return sink_.firstSampleTimestampUs();
}

}  // namespace pet::services
