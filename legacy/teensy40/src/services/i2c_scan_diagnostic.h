#pragma once

#include <Arduino.h>

namespace pet::services {

// Minimal bus discovery. It initializes no sensor library and configures no
// device: it only reports the idle level of SDA and SCL, whether the PCA9548A
// answers, and which addresses respond on each multiplexer channel. Use it to
// separate a wiring or bus problem from a sensor configuration problem.
class I2cScanDiagnostic {
 public:
  static constexpr uint8_t kMuxChannelCount = 8;
  static constexpr uint8_t kMaxAddressesPerChannel = 8;
  static constexpr uint8_t kFirstAddress = 0x08;
  static constexpr uint8_t kLastAddress = 0x77;

  bool begin();
  void poll();

 private:
  void reportBusLevels(const char* stage) const;
  bool recoverBus() const;
  void resetBus() const;
  bool addressResponds(uint8_t address) const;
  uint8_t scanCurrentSelection(uint8_t* addresses, uint8_t capacity) const;
  void printAddresses(const uint8_t* addresses, uint8_t count) const;
  bool muxResponds() const;
  bool selectChannel(uint8_t channel) const;
  bool disableAllChannels() const;
  void scanOnce();

  uint32_t nextReportMs_ = 0;
  uint32_t round_ = 0;
  bool started_ = false;
};

}  // namespace pet::services
