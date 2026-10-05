#include "services/i2c_scan_diagnostic.h"

#include <Wire.h>

#include "config/constants.h"
#include "config/pins.h"

namespace pet::services {
namespace {

constexpr uint8_t kBusRecoveryPulses = 9;
constexpr uint16_t kBusSettleUs = 10;
constexpr uint16_t kBusHalfPeriodUs = 5;
constexpr uint8_t kProbeRetries = 2;

// Every address this project can legitimately answer on: both ICM-20948
// options and both BMP390 options. The AK09916 is not listed because it lives
// on the auxiliary bus inside each ICM and never appears here.
constexpr uint8_t kTargetedAddresses[] = {0x68, 0x69, 0x76, 0x77};

void printHexByte(uint8_t value) {
  if (value < 0x10) {
    Serial.print('0');
  }
  Serial.print(value, HEX);
}

const char* levelName(uint8_t pin) {
  return digitalRead(pin) == HIGH ? "high" : "low";
}

const char* describeAddress(uint8_t address) {
  switch (address) {
    case 0x68:
    case 0x69:
      return "icm20948";
    case 0x76:
    case 0x77:
      return "bmp390";
    default:
      return "unknown";
  }
}

}  // namespace

bool I2cScanDiagnostic::begin() {
  Serial.println();
  Serial.println("PBIC / Pet Analytics - I2C discovery only");
  Serial.print("I2C_SCAN_PINS sda=");
  Serial.print(pins::kI2cSda);
  Serial.print(" scl=");
  Serial.print(pins::kI2cScl);
  Serial.print(" clock_hz=");
  Serial.print(config::kI2cClockHz);
  Serial.print(" mux_address=0x");
  printHexByte(config::kPca9548aAddress);
  Serial.print(" full_sweep=");
  Serial.println(config::kI2cScanFullSweepEnabled ? 1 : 0);
  Serial.println(
      "I2C_SCAN_NOTE no sensor library is initialized in this mode");

  resetBus();
  started_ = true;
  nextReportMs_ = millis();
  return true;
}

void I2cScanDiagnostic::poll() {
  if (!started_ || static_cast<int32_t>(millis() - nextReportMs_) < 0) {
    return;
  }
  scanOnce();
  nextReportMs_ = millis() + config::kI2cScanDiagnosticIntervalMs;
}

void I2cScanDiagnostic::reportBusLevels(const char* stage) const {
  pinMode(pins::kI2cScl, INPUT_PULLUP);
  pinMode(pins::kI2cSda, INPUT_PULLUP);
  delayMicroseconds(kBusSettleUs);
  Serial.print("I2C_BUS_LEVELS stage=");
  Serial.print(stage);
  Serial.print(" sda=");
  Serial.print(levelName(pins::kI2cSda));
  Serial.print(" scl=");
  Serial.println(levelName(pins::kI2cScl));
}

// A device interrupted mid-byte can hold SDA low forever. Resetting the Teensy
// does not reset the sensors, so the line stays down across a firmware upload.
// Clocking SCL lets the device finish the byte it was sending.
bool I2cScanDiagnostic::recoverBus() const {
  pinMode(pins::kI2cScl, INPUT_PULLUP);
  pinMode(pins::kI2cSda, INPUT_PULLUP);
  delayMicroseconds(kBusSettleUs);
  if (digitalRead(pins::kI2cSda) == HIGH) {
    return true;
  }

  for (uint8_t pulse = 0; pulse < kBusRecoveryPulses; ++pulse) {
    pinMode(pins::kI2cScl, OUTPUT);
    digitalWrite(pins::kI2cScl, LOW);
    delayMicroseconds(kBusHalfPeriodUs);
    pinMode(pins::kI2cScl, INPUT_PULLUP);
    delayMicroseconds(kBusHalfPeriodUs);
    if (digitalRead(pins::kI2cSda) == HIGH) {
      break;
    }
  }

  // Manual STOP: pull SDA low, then release it while SCL stays high.
  pinMode(pins::kI2cSda, OUTPUT);
  digitalWrite(pins::kI2cSda, LOW);
  delayMicroseconds(kBusHalfPeriodUs);
  pinMode(pins::kI2cSda, INPUT_PULLUP);
  delayMicroseconds(kBusHalfPeriodUs);
  return digitalRead(pins::kI2cSda) == HIGH;
}

// Re-initializing the Teensy peripheral alone cannot free a line that a sensor
// is holding down, so every reset releases the bus by hand first.
void I2cScanDiagnostic::resetBus() const {
  Wire.end();
  recoverBus();
  Wire.begin();
  Wire.setClock(config::kI2cClockHz);
}

bool I2cScanDiagnostic::addressResponds(uint8_t address) const {
  for (uint8_t attempt = 0; attempt < kProbeRetries; ++attempt) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      return true;
    }
  }
  return false;
}

uint8_t I2cScanDiagnostic::scanCurrentSelection(uint8_t* addresses,
                                                uint8_t capacity) const {
  uint8_t found = 0;
  if (config::kI2cScanFullSweepEnabled) {
    for (uint8_t address = kFirstAddress; address <= kLastAddress; ++address) {
      if (address == config::kPca9548aAddress) {
        continue;
      }
      if (!addressResponds(address)) {
        continue;
      }
      if (found < capacity) {
        addresses[found] = address;
      }
      ++found;
    }
    return found;
  }

  for (const uint8_t address : kTargetedAddresses) {
    if (!addressResponds(address)) {
      continue;
    }
    if (found < capacity) {
      addresses[found] = address;
    }
    ++found;
  }
  return found;
}

bool I2cScanDiagnostic::muxResponds() const {
  return addressResponds(config::kPca9548aAddress);
}

bool I2cScanDiagnostic::selectChannel(uint8_t channel) const {
  Wire.beginTransmission(config::kPca9548aAddress);
  Wire.write(static_cast<uint8_t>(1U << channel));
  if (Wire.endTransmission() != 0) {
    return false;
  }
  delayMicroseconds(config::kPcaChannelSettleUs);
  return true;
}

bool I2cScanDiagnostic::disableAllChannels() const {
  Wire.beginTransmission(config::kPca9548aAddress);
  Wire.write(static_cast<uint8_t>(0x00));
  return Wire.endTransmission() == 0;
}

void I2cScanDiagnostic::printAddresses(const uint8_t* addresses,
                                       uint8_t count) const {
  Serial.print(" count=");
  Serial.print(count);
  Serial.print(" addresses=");
  const uint8_t shown = count < kMaxAddressesPerChannel
                            ? count
                            : kMaxAddressesPerChannel;
  for (uint8_t i = 0; i < shown; ++i) {
    if (i > 0) {
      Serial.print(',');
    }
    Serial.print("0x");
    printHexByte(addresses[i]);
    Serial.print(':');
    Serial.print(describeAddress(addresses[i]));
  }
  Serial.println();
}

void I2cScanDiagnostic::scanOnce() {
  uint8_t addresses[kMaxAddressesPerChannel]{};
  ++round_;

  Serial.print("I2C_SCAN_ROUND round=");
  Serial.print(round_);
  Serial.print(" uptime_ms=");
  Serial.println(millis());

  Wire.end();
  reportBusLevels("round_start");
  const bool released = recoverBus();
  Wire.begin();
  Wire.setClock(config::kI2cClockHz);
  if (!released) {
    Serial.println(
        "I2C_SCAN_HINT sda_held_low: a device is stuck or SDA is shorted to "
        "GND; power cycle the sensors, a firmware upload alone does not reset "
        "them");
  }

  const bool muxPresent = muxResponds();
  Serial.print("I2C_SCAN_MUX address=0x");
  printHexByte(config::kPca9548aAddress);
  Serial.print(" present=");
  Serial.println(muxPresent ? 1 : 0);

  if (!muxPresent) {
    Serial.println(
        "I2C_SCAN_HINT mux_absent: check VCC, GND, SDA on pin 18 and SCL on "
        "pin 19 at the PCA9548A itself");
    Serial.println("I2C_SCAN_END devices=0");
    return;
  }

  uint8_t total = 0;
  for (uint8_t channel = 0; channel < kMuxChannelCount; ++channel) {
    resetBus();
    Serial.print("I2C_SCAN_CHANNEL channel=");
    Serial.print(channel);
    if (!selectChannel(channel)) {
      Serial.println(" selected=0 count=0 addresses=");
      continue;
    }
    const uint8_t count = scanCurrentSelection(addresses, sizeof(addresses));
    total = static_cast<uint8_t>(total + count);
    Serial.print(" selected=1");
    printAddresses(addresses, count);
  }

  resetBus();
  disableAllChannels();
  Serial.print("I2C_SCAN_END devices=");
  Serial.println(total);
}

}  // namespace pet::services
