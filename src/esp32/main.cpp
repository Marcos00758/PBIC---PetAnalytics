#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "config/constants.h"
#include "config/esp32_runtime.h"
#include "config/pins.h"
#include "drivers/bmp390.h"
#include "drivers/icm20948.h"
#include "drivers/pca9548a.h"
#include "services/imu_acquisition.h"
#include "services/sd_logger.h"
#include "utils/esp32_reset_reason.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "The integrated acquisition entry point requires ESP32-S3"
#endif
static_assert(portNUM_PROCESSORS == 2, "Dual-core FreeRTOS required");
static_assert(configTICK_RATE_HZ >= 1000, "Acquisition yield requires a 1 ms tick");

namespace {
using namespace pet;
constexpr EventBits_t kTasksReady = BIT0;
constexpr EventBits_t kStartupFailed = BIT1;
constexpr EventBits_t kAcquisitionStart = BIT2;
constexpr EventBits_t kRecording = BIT3;

struct SensorSetup {
  services::SdSessionMetadata metadata{};
  bool ready = false;
  bool i2cReady = false;
  bool pcaReady = false;
  bool pcaDisabled = false;
  uint8_t icmAddress[data::kIcmCount]{};
  uint8_t icmWhoAmI[data::kIcmCount]{};
  uint8_t icmMagWhoAmI[data::kIcmCount]{};
  bool bmpInitialized[data::kBmpCount]{};
  uint8_t bmpChipId[data::kBmpCount]{};
};

// Explicit static storage: internal SRAM, not PSRAM or per-sample allocations.
StaticQueue_t packetQueueControl, snapshotQueueControl, sensorSetupQueueControl;
alignas(4) uint8_t packetQueueStorage[
    config::kPacketQueueCapacity * sizeof(data::ImuPacket)];
alignas(4) uint8_t snapshotQueueStorage[sizeof(services::AcquisitionCounters)];
alignas(4) uint8_t sensorSetupQueueStorage[sizeof(SensorSetup)];
StaticEventGroup_t eventsControl;
QueueHandle_t packets, snapshots, sensorSetups;
EventGroupHandle_t events;
TaskHandle_t producerHandle, consumerHandle;

drivers::Pca9548a mux(Wire, config::kPca9548aAddress,
                      config::kPcaChannelSettleUs);
drivers::Icm20948 icm0(mux, Wire, config::kIcm0Channel, 100);
drivers::Icm20948 icm1(mux, Wire, config::kIcm1Channel, 110);
drivers::Icm20948 icm2(mux, Wire, config::kIcm2Channel, 120);
drivers::Icm20948* const icms[] = {&icm0, &icm1, &icm2};
drivers::Bmp390 bmp0(mux, Wire, config::kBmp0Channel);
drivers::Bmp390 bmp1(mux, Wire, config::kBmp1Channel);
drivers::Bmp390* const bmps[] = {&bmp0, &bmp1};
services::ImuAcquisition acquisition(
    icm0, icm1, icm2, bmp0, bmp1, config::kImuSamplePeriodUs,
    config::kBmpSamplesPerImuSample, config::kMagSamplesPerImuSample);
services::SdLogger logger;

bool waitForStartup() {
  return !(xEventGroupWaitBits(events, kTasksReady | kStartupFailed,
                              pdFALSE, pdFALSE, portMAX_DELAY) & kStartupFailed);
}

SensorSetup initializeSensors() {
  SensorSetup result;
  result.i2cReady = Wire.begin(pins::kI2cSda, pins::kI2cScl, config::kI2cClockHz);
  Wire.setTimeOut(config::kI2cTimeoutMs);
  result.pcaReady = result.i2cReady && mux.begin();
  result.ready = result.pcaReady;
  if (!result.ready) return result;
  for (size_t i = 0; i < data::kIcmCount; ++i) {
    const bool ok = icms[i]->begin();
    result.metadata.icmReady[i] = ok;
    result.metadata.icmAccelConfig[i] = icms[i]->accelConfig();
    result.metadata.icmGyroConfig[i] = icms[i]->gyroConfig();
    result.metadata.icmMagMode[i] = icms[i]->magnetometerMode();
    result.icmAddress[i] = icms[i]->address();
    result.icmWhoAmI[i] = icms[i]->whoAmI();
    result.icmMagWhoAmI[i] = icms[i]->magnetometerWhoAmI();
    result.ready = ok && result.ready;
  }
  for (size_t i = 0; i < data::kBmpCount; ++i) {
    const bool ok = bmps[i]->begin();
    const bool sampling = ok && bmps[i]->startRawSampling25Hz();
    result.bmpInitialized[i] = ok;
    result.bmpChipId[i] = bmps[i]->chipId();
    result.metadata.bmpConfiguration[i] = bmps[i]->configuration();
    result.metadata.bmpReady[i] = sampling;
    result.metadata.bmpAddress[i] = bmps[i]->address();
    result.metadata.bmpNvmValid[i] =
        sampling && bmps[i]->readNvm(result.metadata.bmpNvm[i]);
    result.ready = sampling && result.metadata.bmpNvmValid[i] && result.ready;
  }
  result.pcaDisabled = mux.disableAllChannels();
  result.ready = result.pcaDisabled && result.ready;
  return result;
}

// Report the saved boot result only; never read drivers or buses from core 0.
void reportSensorSetup(const SensorSetup& sensors) {
  const auto& metadata = sensors.metadata;
  constexpr uint8_t icmChannels[] = {
      config::kIcm0Channel, config::kIcm1Channel, config::kIcm2Channel};
  constexpr uint8_t bmpChannels[] = {config::kBmp0Channel, config::kBmp1Channel};
  Serial.printf("SENSOR_INIT_SUMMARY firmware=%s source=boot_snapshot sensors_ready=%u\n",
                config::kFirmwareVersion, sensors.ready);
  Serial.printf("I2C_INIT ok=%u sda=%u scl=%u hz=%lu timeout_ms=%lu init_core=%u\n",
                sensors.i2cReady, pins::kI2cSda, pins::kI2cScl,
                static_cast<unsigned long>(config::kI2cClockHz),
                static_cast<unsigned long>(config::kI2cTimeoutMs), config::kAcquisitionCore);
  Serial.printf("PCA_INIT address=0x%02x ok=%u final_disable_attempted=%u disable_all=%u\n",
                config::kPca9548aAddress, sensors.pcaReady, sensors.pcaReady,
                sensors.pcaDisabled);
  if (!sensors.pcaReady) {
    Serial.printf("SENSOR_INIT_FAILED device=%s acquisition_enabled=0 sensors_skipped=1\n",
                  sensors.i2cReady ? "PCA9548A" : "I2C");
    return;
  }
  for (size_t i = 0; i < data::kIcmCount; ++i) {
    Serial.printf("ICM_INIT index=%u channel=%u ok=%u address=0x%02x "
                  "who=0x%02x mag_wia2=0x%02x accel_config=0x%02x "
                  "gyro_config=0x%02x mag_mode=0x%02x config_verified=%u\n",
                  unsigned(i), icmChannels[i], metadata.icmReady[i], sensors.icmAddress[i],
                  sensors.icmWhoAmI[i], sensors.icmMagWhoAmI[i],
                  metadata.icmAccelConfig[i], metadata.icmGyroConfig[i],
                  metadata.icmMagMode[i], metadata.icmReady[i]);
  }
  for (size_t i = 0; i < data::kBmpCount; ++i) {
    const auto& cfg = metadata.bmpConfiguration[i];
    Serial.printf("BMP_INIT index=%u channel=%u ok=%u address=0x%02x "
                  "chip_id=0x%02x raw25hz=%u nvm_valid=%u "
                  "readback_ok=%u config_verified=%u pwr_ctrl=0x%02x "
                  "osr=0x%02x odr=0x%02x iir_config=0x%02x error_reg=0x%02x\n", unsigned(i),
                  bmpChannels[i], sensors.bmpInitialized[i], metadata.bmpAddress[i],
                  sensors.bmpChipId[i], metadata.bmpReady[i], metadata.bmpNvmValid[i],
                  cfg.readbackOk, cfg.verified, cfg.powerControl, cfg.oversampling,
                  cfg.outputDataRate, cfg.iirConfig, cfg.error);
  }
}

void producer(void*) {
  if (!waitForStartup()) { vTaskDelete(nullptr); return; }
  const SensorSetup sensors = initializeSensors();
  reportSensorSetup(sensors);
  xQueueOverwrite(sensorSetups, &sensors);
  if (!sensors.ready) { vTaskDelete(nullptr); return; }
  // SD session setup precedes acquisition. No packets accumulate during setup.
  xEventGroupWaitBits(events, kAcquisitionStart, pdFALSE, pdTRUE, portMAX_DELAY);
  acquisition.start(micros());
  uint32_t drops = 0, unavailable = 0, highWater = 0;
  for (;;) {
    data::ImuPacket packet{};
    if (acquisition.poll(packet)) {
      if (!(xEventGroupGetBits(events) & kRecording)) {
        ++unavailable;
      } else if (xQueueSend(packets, &packet, 0) != pdPASS) {
        // Reject the new packet; sequence advances and records the gap.
        ++drops;
        highWater = config::kPacketQueueCapacity;
      }
      const uint32_t depth = uxQueueMessagesWaiting(packets);
      if (depth > highWater) highWater = depth;
    }
    services::AcquisitionCounters snapshot = acquisition.counters();
    snapshot.queueDroppedPackets = drops;
    snapshot.storageUnavailablePackets = unavailable;
    snapshot.queueHighWaterPackets = highWater;
    xQueueOverwrite(snapshots, &snapshot);
    vTaskDelay(1);
  }
}

void report(const services::AcquisitionCounters& counters,
            uint32_t discardedAfterFailure, bool sensorsReady) {
  const auto& sd = logger.counters();
  Serial.printf("ACQUISITION_STATUS sensors_ready=%u recording=%u "
                "produced=%lu missed=%lu failed_rounds=%lu queue_depth=%u "
                "queue_max=%lu queue_drops=%lu storage_unavailable=%lu "
                "pending_discarded=%lu lateness_max_us=%lu acquisition_max_us=%lu "
                "sd_bytes=%llu sd_write_failures=%lu sd_write_max_us=%lu "
                "sd_sync_max_us=%lu producer_stack_free_bytes=%u "
                "consumer_stack_free_bytes=%u\n",
                sensorsReady, logger.sessionActive(),
                static_cast<unsigned long>(counters.packetsProduced),
                static_cast<unsigned long>(counters.missedScheduleReadings),
                static_cast<unsigned long>(counters.failedAcquisitionRounds),
                unsigned(uxQueueMessagesWaiting(packets)),
                static_cast<unsigned long>(counters.queueHighWaterPackets),
                static_cast<unsigned long>(counters.queueDroppedPackets),
                static_cast<unsigned long>(counters.storageUnavailablePackets),
                static_cast<unsigned long>(discardedAfterFailure),
                static_cast<unsigned long>(counters.maxScheduleLatenessUs),
                static_cast<unsigned long>(counters.maxAcquisitionDurationUs),
                static_cast<unsigned long long>(sd.bytesWritten),
                static_cast<unsigned long>(sd.writeFailures),
                static_cast<unsigned long>(sd.maxWriteDurationUs),
                static_cast<unsigned long>(sd.maxFlushDurationUs),
                sensorsReady ? unsigned(uxTaskGetStackHighWaterMark(producerHandle)) : 0,
                unsigned(uxTaskGetStackHighWaterMark(nullptr)));
  Serial.printf("I2C_FAILURES icm=%lu,%lu,%lu bmp=%lu,%lu mag=%lu,%lu,%lu\n",
                static_cast<unsigned long>(counters.i2cFailures[0]),
                static_cast<unsigned long>(counters.i2cFailures[1]),
                static_cast<unsigned long>(counters.i2cFailures[2]),
                static_cast<unsigned long>(counters.bmpI2cFailures[0]),
                static_cast<unsigned long>(counters.bmpI2cFailures[1]),
                static_cast<unsigned long>(counters.magI2cFailures[0]),
                static_cast<unsigned long>(counters.magI2cFailures[1]),
                static_cast<unsigned long>(counters.magI2cFailures[2]));
}

void consumer(void*) {
  if (!waitForStartup()) { vTaskDelete(nullptr); return; }
  SensorSetup sensors;
  // Only the producer initializes sensors. No shared driver access from core 0.
  xQueueReceive(sensorSetups, &sensors, portMAX_DELAY);
  services::AcquisitionCounters counters{};
  const services::AudioCaptureCounters noAudio{};
  bool ready = false;
  if (sensors.ready) {
    ready = logger.beginCard() && logger.beginSession(sensors.metadata, counters, noAudio)
            && logger.finalizeInitialSessionSetup(sensors.metadata, counters, noAudio);
  }
  if (ready) xEventGroupSetBits(events, kRecording);
  Serial.printf("ACQUISITION_START sensors_ready=%u recording=%u folder=%s "
                "packet_bytes=%u rate_hz=%lu producer_core=%u consumer_core=%u "
                "queue_packets=%u queue_bytes=%u spi_mhz=%u audio=0 software_filters=0\n",
                sensors.ready, ready, ready ? logger.sessionFolder() : "none",
                unsigned(sizeof(data::ImuPacket)),
                static_cast<unsigned long>(config::kImuSampleRateHz),
                config::kAcquisitionCore, config::kStorageCore,
                unsigned(config::kPacketQueueCapacity), unsigned(sizeof(packetQueueStorage)),
                config::kSdSpiClockMHz);
  xEventGroupSetBits(events, kAcquisitionStart);
  uint32_t lastReport = millis(), discarded = 0;
  for (;;) {
    xQueuePeek(snapshots, &counters, 0);  // Copy, never a reference to live producer state.
    data::ImuPacket packet{};
    if (logger.sessionActive()) {
      // Leave packets in the RTOS queue while the logger's private buffer is full.
      // One enqueue then service per pass prevents starving writes/maintenance.
      if (logger.canEnqueuePacket() &&
          xQueueReceive(packets, &packet, pdMS_TO_TICKS(1)) == pdPASS) {
        logger.enqueue(packet);
      }
      logger.service(counters, noAudio);
      if (!logger.sessionActive()) xEventGroupClearBits(events, kRecording);
    } else {
      if (xQueueReceive(packets, &packet, pdMS_TO_TICKS(1)) == pdPASS) ++discarded;
    }
    logger.updateFailureIndicator();
    if (millis() - lastReport >= config::kRuntimeReportMs) {
      reportSensorSetup(sensors);
      report(counters, discarded, sensors.ready);
      lastReport = millis();
    }
    vTaskDelay(1);
  }
}
}  // namespace

void setup() {
  Serial.begin(pet::config::kSerialBaud);
  // Bounded wait: autonomous recording must not require a host connection.
  const uint32_t start = millis();
  while (!Serial && millis() - start < 1500) delay(10);
  Serial.printf("PBIC_STAGE=esp32s3_acquisition firmware=%s reset_reason=%d reset=%s "
                "flash_bytes=%lu psram_bytes=%lu\n", pet::config::kFirmwareVersion,
                int(esp_reset_reason()), pet::utils::resetReasonName(esp_reset_reason()),
                static_cast<unsigned long>(ESP.getFlashChipSize()),
                static_cast<unsigned long>(ESP.getPsramSize()));
  packets = xQueueCreateStatic(pet::config::kPacketQueueCapacity,
                              sizeof(pet::data::ImuPacket), packetQueueStorage,
                              &packetQueueControl);
  snapshots = xQueueCreateStatic(1, sizeof(pet::services::AcquisitionCounters),
                                snapshotQueueStorage, &snapshotQueueControl);
  sensorSetups = xQueueCreateStatic(1, sizeof(SensorSetup), sensorSetupQueueStorage,
                                  &sensorSetupQueueControl);
  events = xEventGroupCreateStatic(&eventsControl);
  if (!packets || !snapshots || !sensorSetups || !events) {
    Serial.println("ACQUISITION_SETUP_FAILED reason=rtos_objects");
    return;
  }
  const BaseType_t storageCreated = xTaskCreatePinnedToCore(
      consumer, "pbic-sd", pet::config::kStorageStackBytes, nullptr,
      pet::config::kStoragePriority, &consumerHandle, pet::config::kStorageCore);
  const BaseType_t producerCreated = storageCreated == pdPASS ? xTaskCreatePinnedToCore(
      producer, "pbic-sensors", pet::config::kAcquisitionStackBytes, nullptr,
      pet::config::kAcquisitionPriority, &producerHandle, pet::config::kAcquisitionCore) : pdFAIL;
  if (storageCreated != pdPASS || producerCreated != pdPASS) {
    Serial.println("ACQUISITION_SETUP_FAILED reason=task_allocation");
    xEventGroupSetBits(events, kStartupFailed);
    return;
  }
  xEventGroupSetBits(events, kTasksReady);
}

void loop() { delay(100); }
