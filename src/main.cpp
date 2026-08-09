#include <Arduino.h>
#include <Wire.h>

#include "config/constants.h"
#include "config/pins.h"
#include "data/imu_packet.h"
#include "drivers/bmp390.h"
#include "drivers/icm20948.h"
#include "drivers/pca9548a.h"
#include "services/bmp_diagnostic.h"
#include "services/imu_acquisition.h"

namespace {

pet::drivers::Pca9548a mux(Wire, pet::config::kPca9548aAddress,
                           pet::config::kPcaChannelSettleUs);
pet::drivers::Icm20948 icm0(mux, Wire, pet::config::kIcm0Channel, 100);
pet::drivers::Icm20948 icm1(mux, Wire, pet::config::kIcm1Channel, 110);
pet::drivers::Icm20948 icm2(mux, Wire, pet::config::kIcm2Channel, 120);
pet::drivers::Icm20948* const icms[] = {&icm0, &icm1, &icm2};
pet::drivers::Bmp390 bmp0(mux, Wire, pet::config::kBmp0Channel);
pet::drivers::Bmp390 bmp1(mux, Wire, pet::config::kBmp1Channel);
pet::drivers::Bmp390* const bmps[] = {&bmp0, &bmp1};
pet::services::ImuAcquisition acquisition(
    icm0, icm1, icm2, pet::config::kImuSamplePeriodUs);

bool acquisitionReady = false;

void printHexByte(uint8_t value) {
  if (value < 0x10) {
    Serial.print('0');
  }
  Serial.print(value, HEX);
}

bool initializeSensor(pet::drivers::Icm20948& icm) {
  Serial.print("ICM channel ");
  Serial.print(icm.muxChannel());

  if (!icm.begin()) {
    Serial.println(" FAILED (tested 0x69 and 0x68; expected WHO_AM_I=0xEA)");
    return false;
  }

  Serial.print(" OK address=0x");
  printHexByte(icm.address());
  Serial.print(" WHO_AM_I=0x");
  printHexByte(icm.whoAmI());
  Serial.println(" accel=+/-2g gyro=+/-250dps");
  return true;
}

bool initializeBmp(pet::drivers::Bmp390& bmp) {
  Serial.print("BMP390 channel ");
  Serial.print(bmp.muxChannel());

  if (!bmp.begin()) {
    Serial.println(" FAILED (tested 0x77 and 0x76; expected CHIP_ID=0x60)");
    return false;
  }

  Serial.print(" OK address=0x");
  printHexByte(bmp.address());
  Serial.print(" CHIP_ID=0x");
  printHexByte(bmp.chipId());
  Serial.println();
  return true;
}

void printBmpStatistics(size_t index,
                        const pet::services::BmpDiagnosticSensorResult& result) {
  Serial.print("BMP_STATS index=");
  Serial.print(index);
  Serial.print(" channel=");
  Serial.print(bmps[index]->muxChannel());
  Serial.print(" samples=");
  Serial.print(result.pressurePa.count);
  Serial.print(" failures=");
  Serial.print(result.readFailures);
  Serial.print(" pressure_mean_pa=");
  Serial.print(result.pressurePa.mean, 2);
  Serial.print(" pressure_min_pa=");
  Serial.print(result.pressurePa.minimum, 2);
  Serial.print(" pressure_max_pa=");
  Serial.print(result.pressurePa.maximum, 2);
  Serial.print(" pressure_stddev_pa=");
  Serial.print(result.pressurePa.standardDeviation, 3);
  Serial.print(" temperature_mean_c=");
  Serial.print(result.temperatureC.mean, 2);
  Serial.print(" temperature_stddev_c=");
  Serial.println(result.temperatureC.standardDeviation, 3);
}

}  // namespace

void setup() {
  Serial.begin(pet::config::kSerialBaud);
  const uint32_t serialWaitStart = millis();
  while (!Serial &&
         millis() - serialWaitStart < pet::config::kSerialWaitTimeoutMs) {
  }

  Serial.println();
  Serial.println("PBIC / Pet Analytics - synchronized ICM acquisition");
  Serial.print("Wire pins SDA=");
  Serial.print(pet::pins::kI2cSda);
  Serial.print(" SCL=");
  Serial.print(pet::pins::kI2cScl);
  Serial.print(" clock_hz=");
  Serial.println(pet::config::kI2cClockHz);

  Wire.begin();
  Wire.setClock(pet::config::kI2cClockHz);

  if (!mux.begin()) {
    Serial.println("PCA9548A FAILED at address 0x70");
    return;
  }
  Serial.println("PCA9548A OK at address 0x70");

  bool sensorsReady = true;
  for (pet::drivers::Icm20948* icm : icms) {
    sensorsReady = initializeSensor(*icm) && sensorsReady;
  }
  bool bmpsReady = true;
  for (pet::drivers::Bmp390* bmp : bmps) {
    bmpsReady = initializeBmp(*bmp) && bmpsReady;
  }
  if (bmpsReady) {
    Serial.print("BMP_DIAGNOSTIC_START rounds=");
    Serial.print(pet::config::kBmpDiagnosticRounds);
    Serial.print(" warmup_rounds=");
    Serial.print(pet::config::kBmpDiagnosticWarmupRounds);
    Serial.print(" period_ms=");
    Serial.println(pet::config::kBmpDiagnosticPeriodUs / 1000);

    const pet::services::BmpDiagnosticResult diagnostic =
        pet::services::runBmpDiagnostic(
            bmp0, bmp1, pet::config::kBmpDiagnosticRounds,
            pet::config::kBmpDiagnosticWarmupRounds,
            pet::config::kBmpDiagnosticPeriodUs);
    for (size_t i = 0; i < 2; ++i) {
      printBmpStatistics(i, diagnostic.sensors[i]);
    }
    Serial.print("BMP_DIAGNOSTIC_END elapsed_ms=");
    Serial.println(diagnostic.elapsedMs);
  }
  mux.disableAllChannels();

  if (!sensorsReady) {
    Serial.println("Acquisition disabled because at least one ICM failed.");
    return;
  }

  Serial.print("BINARY_STREAM_START packet_size=");
  Serial.print(sizeof(pet::data::ImuPacket));
  Serial.print(" sample_rate_hz=");
  Serial.println(pet::config::kImuSampleRateHz);

  acquisition.start(micros());
  acquisitionReady = true;
}

void loop() {
  if (!acquisitionReady) {
    return;
  }

  pet::data::ImuPacket packet{};
  if (!acquisition.poll(packet)) {
    return;
  }

  if (Serial && Serial.availableForWrite() >=
                    static_cast<int>(sizeof(pet::data::ImuPacket))) {
    Serial.write(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
  } else {
    acquisition.recordUsbDrop();
  }
}
