#include <Arduino.h>
#include <Wire.h>

#include "config/constants.h"
#include "config/pins.h"
#include "drivers/icm20948.h"
#include "drivers/pca9548a.h"

namespace {

pet::drivers::Pca9548a mux(Wire, pet::config::kPca9548aAddress,
                           pet::config::kPcaChannelSettleUs);
pet::drivers::Icm20948 icm0(mux, Wire, pet::config::kIcm0Channel, 100);
pet::drivers::Icm20948 icm1(mux, Wire, pet::config::kIcm1Channel, 110);
pet::drivers::Icm20948 icm2(mux, Wire, pet::config::kIcm2Channel, 120);

pet::drivers::Icm20948* const icms[] = {&icm0, &icm1, &icm2};
uint32_t lastDiagnosticMs = 0;
bool muxReady = false;

void printHexByte(uint8_t value) {
  if (value < 0x10) {
    Serial.print('0');
  }
  Serial.print(value, HEX);
}

void printSensorStatus(pet::drivers::Icm20948& icm) {
  Serial.print("ICM channel ");
  Serial.print(icm.muxChannel());

  if (!icm.begin()) {
    Serial.println(" FAILED (tested 0x69 and 0x68; expected WHO_AM_I=0xEA)");
    return;
  }

  Serial.print(" OK address=0x");
  printHexByte(icm.address());
  Serial.print(" WHO_AM_I=0x");
  printHexByte(icm.whoAmI());
  Serial.println(" accel=+/-2g gyro=+/-250dps");
}

void printVector(const pet::drivers::Vector3f& value) {
  Serial.print(value.x, 4);
  Serial.print(',');
  Serial.print(value.y, 4);
  Serial.print(',');
  Serial.print(value.z, 4);
}

void printSample(pet::drivers::Icm20948& icm) {
  if (!icm.initialized()) {
    return;
  }

  pet::drivers::Icm20948Sample sample{};
  Serial.print("ICM ch");
  Serial.print(icm.muxChannel());
  Serial.print(' ');

  if (!icm.read(sample)) {
    Serial.println("READ_FAILED");
    return;
  }

  Serial.print("t_us=");
  Serial.print(sample.timestampUs);
  Serial.print(" accel_mps2=");
  printVector(sample.accelerationMps2);
  Serial.print(" gyro_dps=");
  printVector(sample.gyroDps);
  Serial.println();
}

}  // namespace

void setup() {
  Serial.begin(pet::config::kSerialBaud);
  const uint32_t serialWaitStart = millis();
  while (!Serial &&
         millis() - serialWaitStart < pet::config::kSerialWaitTimeoutMs) {
  }

  Serial.println();
  Serial.println("PBIC / Pet Analytics - I2C and ICM-20948 diagnostic");
  Serial.print("Wire pins SDA=");
  Serial.print(pet::pins::kI2cSda);
  Serial.print(" SCL=");
  Serial.print(pet::pins::kI2cScl);
  Serial.print(" clock_hz=");
  Serial.println(pet::config::kI2cClockHz);

  Wire.begin();
  Wire.setClock(pet::config::kI2cClockHz);

  muxReady = mux.begin();
  if (!muxReady) {
    Serial.println("PCA9548A FAILED at address 0x70");
    return;
  }

  Serial.println("PCA9548A OK at address 0x70");
  for (pet::drivers::Icm20948* icm : icms) {
    printSensorStatus(*icm);
  }

  mux.disableAllChannels();
  Serial.println("Diagnostic initialized; one sample per sensor every second.");
}

void loop() {
  if (!muxReady) {
    return;
  }

  const uint32_t now = millis();
  if (now - lastDiagnosticMs < pet::config::kDiagnosticReadIntervalMs) {
    return;
  }
  lastDiagnosticMs = now;

  for (pet::drivers::Icm20948* icm : icms) {
    printSample(*icm);
  }
}
