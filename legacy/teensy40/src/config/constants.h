#pragma once

#include <Arduino.h>

namespace pet::config {

constexpr uint32_t kSerialBaud = 115200;
constexpr bool kUsbBinaryStreamEnabled = false;
constexpr bool kMicrophoneDiagnosticEnabled = false;
constexpr bool kAudioSdDiagnosticEnabled = false;
constexpr bool kMicrophoneRecordingEnabled = false;
constexpr bool kPresentationStreamEnabled = true;
constexpr bool kI2cScanDiagnosticEnabled = false;
constexpr uint32_t kI2cScanDiagnosticIntervalMs = 2000;
// A full 0x08..0x77 sweep costs about a hundred unanswered transactions
// per channel and has been observed to hang the bus. The targeted probe
// only asks for the addresses this project actually uses.
constexpr bool kI2cScanFullSweepEnabled = false;
constexpr uint32_t kMicrophoneDiagnosticReportMs = 2000;
constexpr uint8_t kMicrophoneAudioMemoryBlocks = 48;
constexpr uint16_t kMicrophoneQueueBlocks = 512;
constexpr uint32_t kMicrophoneSampleRateHz = 44100;
constexpr uint16_t kMicrophoneBlockSamples = 128;
constexpr uint8_t kMicrophoneChannels = 1;
constexpr uint8_t kMicrophoneBitsPerSample = 16;
constexpr uint32_t kI2cClockHz = 400000;

constexpr char kFirmwareVersion[] = "0.5.1";

constexpr size_t kSdRamBufferBytes = 8192;
constexpr size_t kSdAudioRamBufferBytes = 32768;
constexpr size_t kSdImuWriteBlockBytes = 512;
constexpr size_t kSdAudioWriteBlockBytes = 512;
constexpr uint8_t kSdSpiClockMHz = 12;
constexpr uint64_t kSdFreeSpaceReserveBytes = 4ULL * 1024ULL * 1024ULL;
constexpr uint32_t kSdMinimumRecordingSeconds = 60;
constexpr bool kSdContinuousSessionEnabled = true;
constexpr uint32_t kSdPreallocationSeconds = 10U * 60U;
constexpr bool kSdRotateSessions = false;
constexpr uint32_t kSdPreallocationMarginSeconds = 1;
constexpr uint32_t kSdPacketsPerFlush = 1000;
constexpr uint32_t kSdPacketsPerJournalUpdate = kSdPacketsPerFlush;
constexpr uint32_t kSdPacketsPerStatusUpdate = 18000;
constexpr uint32_t kSdHealthWindowMs = 2000;
constexpr uint32_t kSdWriteRetryMs = 20;
constexpr uint32_t kSdFailureLedDelayMs = 5000;
constexpr bool kSdFailureIndicatorEnabled = true;
constexpr uint32_t kSdFailureLedCycleMs = 1200;
constexpr uint32_t kSdFailureLedPulseMs = 150;
constexpr uint32_t kSdFailureLedSecondPulseMs = 300;
constexpr uint32_t kSdSlowOperationThresholdUs = 10000;
constexpr uint8_t kSdAudioWritesPerImuWrite = 11;
constexpr size_t kSdImuReservedCapacityBytes = 4U * kSdImuWriteBlockBytes;
constexpr size_t kSdAudioReservedCapacityBytes =
    16U * kSdAudioWriteBlockBytes;
constexpr size_t kSdMaintenanceMaxImuBufferedBytes =
    2U * kSdImuWriteBlockBytes;
constexpr size_t kSdMaintenanceMaxAudioBufferedBytes =
    8U * kSdAudioWriteBlockBytes;
static_assert(kSdRamBufferBytes % kSdImuWriteBlockBytes == 0,
              "SD RAM buffer must contain complete write blocks");
static_assert(kSdAudioRamBufferBytes % kSdAudioWriteBlockBytes == 0,
              "audio SD RAM buffer must contain complete write blocks");
static_assert(kMicrophoneBlockSamples * sizeof(int16_t) <=
                  kSdAudioRamBufferBytes,
              "audio SD buffer must contain a complete audio block");
static_assert(kSdImuReservedCapacityBytes < kSdRamBufferBytes,
              "IMU reservation must leave writable buffer capacity");
static_assert(kSdAudioReservedCapacityBytes < kSdAudioRamBufferBytes,
              "audio reservation must leave writable buffer capacity");
static_assert(kSdMaintenanceMaxImuBufferedBytes <=
                  kSdRamBufferBytes - kSdImuReservedCapacityBytes,
              "IMU maintenance threshold must be below urgency threshold");
static_assert(kSdMaintenanceMaxAudioBufferedBytes <=
                  kSdAudioRamBufferBytes - kSdAudioReservedCapacityBytes,
              "audio maintenance threshold must be below urgency threshold");

constexpr uint32_t kAudioPreflightDurationMs = 2000;
constexpr int32_t kAudioPreflightMaximumAbsMeanCounts = 1024;
constexpr uint32_t kAudioPreflightMaximumRmsCounts = 4096;
constexpr uint32_t kAudioPreflightMaximumClippingPpm = 100;

constexpr uint32_t kAudioSdDiagnosticPhaseSeconds = 5U * 60U;
constexpr uint32_t kAudioSdDiagnosticDurationSeconds =
    2U * kAudioSdDiagnosticPhaseSeconds;
constexpr size_t kAudioSdDiagnosticBlockBytes[] = {256U, 512U};
constexpr size_t kAudioSdDiagnosticPhaseCount =
    sizeof(kAudioSdDiagnosticBlockBytes) /
    sizeof(kAudioSdDiagnosticBlockBytes[0]);
constexpr uint32_t kAudioSdDiagnosticFlushSeconds = 10U;
constexpr uint32_t kAudioSdDiagnosticJournalSeconds = 30U;
static_assert(kAudioSdDiagnosticPhaseCount == 2,
              "audio SD benchmark requires two phases");
static_assert(kSdAudioRamBufferBytes % kAudioSdDiagnosticBlockBytes[0] == 0 &&
                  kSdAudioRamBufferBytes %
                          kAudioSdDiagnosticBlockBytes[1] ==
                      0,
              "audio SD buffer must contain complete benchmark blocks");
static_assert(!(kMicrophoneDiagnosticEnabled && kAudioSdDiagnosticEnabled),
              "enable only one microphone diagnostic mode");
static_assert(!(kPresentationStreamEnabled && kMicrophoneDiagnosticEnabled) &&
                  !(kPresentationStreamEnabled && kAudioSdDiagnosticEnabled) &&
                  !(kPresentationStreamEnabled && kMicrophoneRecordingEnabled),
              "presentation streaming excludes every audio mode");
static_assert(!(kPresentationStreamEnabled && kUsbBinaryStreamEnabled),
              "presentation streaming already owns the USB binary stream");
static_assert(!(kI2cScanDiagnosticEnabled && kPresentationStreamEnabled) &&
                  !(kI2cScanDiagnosticEnabled && kMicrophoneDiagnosticEnabled) &&
                  !(kI2cScanDiagnosticEnabled && kAudioSdDiagnosticEnabled),
              "enable only one diagnostic mode at a time");

constexpr uint8_t kPca9548aAddress = 0x70;
constexpr uint16_t kPcaChannelSettleUs = 80;

constexpr uint8_t kIcm0Channel = 4;
constexpr uint8_t kIcm1Channel = 5;
constexpr uint8_t kIcm2Channel = 6;
constexpr uint8_t kIcmSensorCount = 3;

constexpr uint8_t kBmp0Channel = 7;
constexpr uint8_t kBmp1Channel = 3;
constexpr uint8_t kBmpCandidateAddresses[] = {0x77, 0x76};
constexpr uint8_t kBmpExpectedChipId = 0x60;
constexpr uint32_t kBmpSampleRateHz = 25;

// Noise reduction of the BMP390. Both mechanisms the sensor offers were off,
// which left pressure noise at its maximum: roughly a few pascal, and one
// pascal is about eight centimetres of altitude.
constexpr bool kBmpOversamplingEnabled = true;
constexpr bool kBmpIirFilterEnabled = true;
// Register OSR 0x1C: bits [2:0] select pressure, bits [5:3] temperature.
// Code 3 means x8 and code 1 means x2. At 25 Hz the conversion then needs
// about 21 ms of the 40 ms period; x16 pressure would need about 37 ms and
// leave no margin for the sensor to settle.
constexpr uint8_t kBmpPressureOversamplingCode = 3;
constexpr uint8_t kBmpTemperatureOversamplingCode = 1;
constexpr uint8_t kBmpPressureOversamplingFactor = 8;
constexpr uint8_t kBmpTemperatureOversamplingFactor = 2;
// Register CONFIG 0x1F: bits [3:1] hold the IIR coefficient. Code 2 selects
// coefficient 3, about four samples of time constant at 25 Hz.
constexpr uint8_t kBmpIirCoefficientCode = 2;
constexpr uint8_t kBmpIirCoefficient = 3;

// Try the Adafruit STEMMA QT default first, then the alternate address.
constexpr uint8_t kIcmCandidateAddresses[] = {0x69, 0x68};
constexpr uint8_t kIcmWhoAmIRegister = 0x00;
constexpr uint8_t kIcmRegisterBankSelect = 0x7F;
constexpr uint8_t kIcmExpectedWhoAmI = 0xEA;
constexpr uint32_t kMagSampleRateHz = 20;
constexpr uint32_t kMagPollRateHz = 25;
constexpr uint8_t kIcmAccelRangeG = 8;
constexpr uint16_t kIcmGyroRangeDps = 2000;

// The ICM low-pass filters also gate the output data rate. With the filter
// bypassed the accelerometer free-runs at 4500 Hz and the gyroscope at 9000 Hz
// and the rate divisors below are ignored, so sampling at 100 Hz folds
// everything above 50 Hz back into the band. Enabling the filters makes the
// divisors effective and provides the anti-aliasing that 100 Hz requires.
constexpr bool kIcmDlpfEnabled = true;
constexpr char kIcmAccelDlpfLabel[] = "50.4";
constexpr char kIcmGyroDlpfLabel[] = "51.2";

// Internal sensor ODRs with the filters enabled: accel ~=102.3 Hz, gyro 100 Hz.
constexpr uint16_t kIcmAccelRateDivisor = 10;
constexpr uint8_t kIcmGyroRateDivisor = 10;

constexpr uint32_t kImuSampleRateHz = 100;
constexpr uint32_t kImuSamplePeriodUs = 1000000UL / kImuSampleRateHz;
constexpr uint32_t kBmpSamplesPerImuSample =
    kImuSampleRateHz / kBmpSampleRateHz;
constexpr uint32_t kMagSamplesPerImuSample =
    kImuSampleRateHz / kMagPollRateHz;
static_assert(kImuSampleRateHz % kBmpSampleRateHz == 0,
              "BMP rate must divide IMU rate");
static_assert(kImuSampleRateHz % kMagPollRateHz == 0,
              "magnetometer poll rate must divide IMU rate");
static_assert(kMagSamplesPerImuSample >= kIcmSensorCount,
              "magnetometer schedule needs one phase per ICM");
static_assert(1000000UL % kImuSampleRateHz == 0,
              "IMU sample period must be an integer number of microseconds");

constexpr uint32_t kPresentationBannerIntervalMs = 5000;

constexpr uint16_t kImuPacketMagic = 0xAA55;
constexpr uint8_t kImuPacketVersion = 4;
constexpr uint8_t kCrc8Polynomial = 0x07;

}  // namespace pet::config
