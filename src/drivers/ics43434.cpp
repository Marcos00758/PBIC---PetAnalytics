#include "drivers/ics43434.h"

#include <math.h>
#include <string.h>

#include "config/constants.h"
#include "config/pins.h"

namespace pet::drivers {
namespace {

constexpr uint32_t kClippingThreshold = 32760;

uint8_t queueCount(uint8_t head, uint8_t tail, uint8_t depth) {
  return head >= tail ? head - tail : depth + head - tail;
}

}  // namespace

AudioRamSink::AudioRamSink() : AudioStream(2, inputQueueArray_) {}

bool AudioRamSink::pop(StereoAudioBlock& block) {
  const uint8_t tail = tail_;
  if (tail == head_) {
    return false;
  }
  memcpy(&block, &queue_[tail], sizeof(block));
  __disable_irq();
  tail_ = static_cast<uint8_t>((tail + 1U) % kQueueDepth);
  __enable_irq();
  return true;
}

uint32_t AudioRamSink::framesSeen() const {
  __disable_irq();
  const uint32_t value = framesSeen_;
  __enable_irq();
  return value;
}

uint32_t AudioRamSink::framesDropped() const {
  __disable_irq();
  const uint32_t value = framesDropped_;
  __enable_irq();
  return value;
}

uint32_t AudioRamSink::incompleteFrames() const {
  __disable_irq();
  const uint32_t value = incompleteFrames_;
  __enable_irq();
  return value;
}

uint8_t AudioRamSink::queueHighWater() const {
  return queueHighWater_;
}

void AudioRamSink::update() {
  audio_block_t* left = receiveReadOnly(0);
  audio_block_t* right = receiveReadOnly(1);
  if (left == nullptr || right == nullptr) {
    if (left != nullptr) {
      release(left);
    }
    if (right != nullptr) {
      release(right);
    }
    ++incompleteFrames_;
    return;
  }

  ++framesSeen_;
  const uint8_t nextHead = static_cast<uint8_t>((head_ + 1U) % kQueueDepth);
  if (nextHead == tail_) {
    ++framesDropped_;
  } else {
    memcpy(queue_[head_].left, left->data, sizeof(queue_[head_].left));
    memcpy(queue_[head_].right, right->data, sizeof(queue_[head_].right));
    head_ = nextHead;
    const uint8_t count = queueCount(head_, tail_, kQueueDepth);
    if (count > queueHighWater_) {
      queueHighWater_ = count;
    }
  }
  release(left);
  release(right);
}

bool Ics43434Diagnostic::begin() {
  static_assert(pins::kMicBclk == 21, "Teensy 4.x AudioInputI2S BCLK is pin 21");
  static_assert(pins::kMicLrclk == 20,
                "Teensy 4.x AudioInputI2S LRCLK is pin 20");
  static_assert(pins::kMicData == 8, "Teensy 4.x AudioInputI2S RX is pin 8");

  AudioMemory(config::kMicrophoneAudioMemoryBlocks);
  static AudioInputI2S input;
  static AudioConnection leftConnection(input, 0, sink_, 0);
  static AudioConnection rightConnection(input, 1, sink_, 1);
  input_ = &input;
  leftConnection_ = &leftConnection;
  rightConnection_ = &rightConnection;

  startedUs_ = micros();
  initialFramesSeen_ = sink_.framesSeen();
  nextReportMs_ = millis() + config::kMicrophoneDiagnosticReportMs;
  started_ = true;

  Serial.println("MIC_DIAGNOSTIC_START library=Teensy_Audio input=AudioInputI2S");
  Serial.print("MIC_I2S pins_bclk=");
  Serial.print(pins::kMicBclk);
  Serial.print(" pins_lrclk=");
  Serial.print(pins::kMicLrclk);
  Serial.print(" pins_data=");
  Serial.print(pins::kMicData);
  Serial.println(" sel=GND expected_channel=left");
  Serial.print("MIC_FORMAT sample_rate_hz=");
  Serial.print(AUDIO_SAMPLE_RATE_EXACT, 1);
  Serial.print(" block_samples=");
  Serial.print(AUDIO_BLOCK_SAMPLES);
  Serial.println(
      " native_bits=24 library_pcm_bits=16 native_low_bits_discarded=8");
  return true;
}

void Ics43434Diagnostic::poll() {
  if (!started_) {
    return;
  }

  StereoAudioBlock block{};
  while (sink_.pop(block)) {
    accumulate(left_, block.left, AUDIO_BLOCK_SAMPLES);
    accumulate(right_, block.right, AUDIO_BLOCK_SAMPLES);
  }

  const uint32_t nowMs = millis();
  if (static_cast<int32_t>(nowMs - nextReportMs_) >= 0) {
    nextReportMs_ = nowMs + config::kMicrophoneDiagnosticReportMs;
    printReport();
  }
}

void Ics43434Diagnostic::accumulate(ChannelStatistics& statistics,
                                    const int16_t* samples, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const int32_t value = samples[i];
    const uint32_t absolute =
        value < 0 ? static_cast<uint32_t>(-value) : static_cast<uint32_t>(value);
    ++statistics.samples;
    statistics.sum += value;
    statistics.sumSquares += static_cast<uint64_t>(value * value);
    statistics.lsbOnes += static_cast<uint16_t>(value) & 1U;
    statistics.clippingSamples += absolute >= kClippingThreshold;
    if (absolute > statistics.maxAbsolute) {
      statistics.maxAbsolute = absolute;
    }
    if (value < statistics.minimum) {
      statistics.minimum = static_cast<int16_t>(value);
    }
    if (value > statistics.maximum) {
      statistics.maximum = static_cast<int16_t>(value);
    }
  }
}

void Ics43434Diagnostic::printReport() {
  const uint32_t framesSeen = sink_.framesSeen();
  const uint32_t elapsedUs = micros() - startedUs_;
  const uint32_t generatedFrames = framesSeen - initialFramesSeen_;
  const double effectiveRate = elapsedUs == 0
                                   ? 0.0
                                   : static_cast<double>(generatedFrames) *
                                         AUDIO_BLOCK_SAMPLES * 1000000.0 /
                                         elapsedUs;

  Serial.print("MIC_TIMING elapsed_s=");
  Serial.print(elapsedUs / 1000000.0, 3);
  Serial.print(" effective_rate_hz=");
  Serial.print(effectiveRate, 2);
  Serial.print(" frames_seen=");
  Serial.print(framesSeen);
  Serial.print(" ram_queue_dropped=");
  Serial.print(sink_.framesDropped());
  Serial.print(" incomplete_frames=");
  Serial.print(sink_.incompleteFrames());
  Serial.print(" queue_high_water=");
  Serial.print(sink_.queueHighWater());
  Serial.print(" audio_memory_used=");
  Serial.print(AudioMemoryUsage());
  Serial.print(" audio_memory_max=");
  Serial.print(AudioMemoryUsageMax());
  Serial.print(" cpu_audio_max_pct=");
  Serial.println(AudioProcessorUsageMax(), 3);

  printChannel("left", left_);
  printChannel("right", right_);

  const double leftRms = acRms(left_);
  const double rightRms = acRms(right_);
  const double ratio = rightRms > 0.0 ? leftRms / rightRms : 1000000.0;
  const bool leftConfirmed = leftRms >= 8.0 && ratio >= 4.0;
  Serial.print("MIC_CHANNEL expected=left detected=");
  Serial.print(leftConfirmed ? "left" : "inconclusive");
  Serial.print(" left_right_db=");
  Serial.print(20.0 * log10(ratio), 2);
  Serial.println(
      " storage_candidate=pcm_s16le decision_requires_real_capture=yes");
}

void Ics43434Diagnostic::printChannel(
    const char* name, const ChannelStatistics& statistics) const {
  const double mean = statistics.samples == 0
                          ? 0.0
                          : static_cast<double>(statistics.sum) /
                                statistics.samples;
  const double rms = acRms(statistics);
  const double rmsDbfs = rms > 0.0 ? 20.0 * log10(rms / 32768.0) : -120.0;
  const double clippingPercent =
      statistics.samples == 0
          ? 0.0
          : 100.0 * statistics.clippingSamples / statistics.samples;
  const double lsbOnesPercent =
      statistics.samples == 0
          ? 0.0
          : 100.0 * statistics.lsbOnes / statistics.samples;

  Serial.print("MIC_CHANNEL_STATS channel=");
  Serial.print(name);
  Serial.print(" samples=");
  Serial.print(static_cast<uint32_t>(statistics.samples));
  Serial.print(" min=");
  Serial.print(statistics.minimum);
  Serial.print(" max=");
  Serial.print(statistics.maximum);
  Serial.print(" mean_counts=");
  Serial.print(mean, 3);
  Serial.print(" ac_rms_counts=");
  Serial.print(rms, 3);
  Serial.print(" rms_dbfs=");
  Serial.print(rmsDbfs, 2);
  Serial.print(" peak_used_bits=");
  Serial.print(signedBitsUsed(statistics.maxAbsolute));
  Serial.print(" lsb_ones_pct=");
  Serial.print(lsbOnesPercent, 2);
  Serial.print(" clipping_samples=");
  Serial.print(statistics.clippingSamples);
  Serial.print(" clipping_pct=");
  Serial.println(clippingPercent, 5);
}

double Ics43434Diagnostic::acRms(
    const ChannelStatistics& statistics) const {
  if (statistics.samples == 0) {
    return 0.0;
  }
  const double mean =
      static_cast<double>(statistics.sum) / statistics.samples;
  const double meanSquare =
      static_cast<double>(statistics.sumSquares) / statistics.samples;
  const double variance = meanSquare - mean * mean;
  return sqrt(variance > 0.0 ? variance : 0.0);
}

uint8_t Ics43434Diagnostic::signedBitsUsed(uint32_t maximumAbsolute) const {
  uint8_t magnitudeBits = 0;
  while (maximumAbsolute > 0) {
    ++magnitudeBits;
    maximumAbsolute >>= 1;
  }
  return static_cast<uint8_t>(magnitudeBits + 1U);
}

}  // namespace pet::drivers
