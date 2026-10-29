"""Exercise the real BMP driver against acknowledged but incorrect registers."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BmpConfigurationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("g++ required for BMP driver checks")
        cls.build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.build.cleanup)
        folder = Path(cls.build.name)
        headers = {
            "Arduino.h": "#pragma once\n#include <stdint.h>\n#include <stddef.h>\ninline void delay(unsigned) {}\n",
            "Wire.h": r"""#pragma once
#include <Arduino.h>
class TwoWire {
 public:
  uint8_t regs[256]{}, tx[2]{}, pointer = 0;
  unsigned written = 0, pending = 0;
  int ignoredWrite = -1, failedWrite = -1, failedRead = -1;
  int overrideRegister = -1;
  uint8_t overrideValue = 0;
  void beginTransmission(uint8_t) { written = 0; }
  size_t write(uint8_t value) { tx[written++] = value; return 1; }
  uint8_t endTransmission(bool = true) {
    if (written == 2) {
      if (tx[0] == failedWrite) return 2;
      if (tx[0] != ignoredWrite) regs[tx[0]] = tx[1];
    } else if (written) pointer = tx[0];
    return 0;
  }
  uint8_t requestFrom(uint8_t, uint8_t count) {
    pending = pointer == failedRead ? 0 : count;
    return pending;
  }
  int available() { return pending; }
  int read() {
    --pending;
    const uint8_t address = pointer++;
    return address == overrideRegister ? overrideValue : regs[address];
  }
};
""",
            "Adafruit_BMP3XX.h": """#pragma once
#include <Wire.h>
class Adafruit_BMP3XX {
 public:
  bool begin_I2C(uint8_t, TwoWire*) { return true; }
  uint8_t chipID() { return 0x60; }
};
""",
            "drivers/pca9548a.h": """#pragma once
#include <Wire.h>
namespace pet { namespace drivers {
class Pca9548a {
 public:
  explicit Pca9548a(TwoWire&) {}
  bool selectChannel(uint8_t) { return true; }
};
}}
""",
            "config/constants.h": """#pragma once
#include <stdint.h>
namespace pet { namespace config {
constexpr uint8_t kBmpCandidateAddresses[] = {0x77, 0x76};
constexpr uint8_t kBmpExpectedChipId = 0x60;
}}
""",
        }
        for name, contents in headers.items():
            path = folder / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
        harness = folder / "test.cpp"
        harness.write_text(r"""
#include <cassert>
#include <string>
#include "drivers/bmp390.h"
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string mode = argv[1];
  TwoWire wire;
  wire.regs[0x1C] = 0x02; // Factory default x4 must be replaced by x1.
  wire.regs[0x1D] = 0x04;
  wire.regs[0x1F] = 0x0E; // Previously enabled IIR must be explicitly disabled.
  if (mode == "osr") wire.ignoredWrite = 0x1C;
  if (mode == "odr") wire.ignoredWrite = 0x1D;
  if (mode == "iir") wire.ignoredWrite = 0x1F;
  if (mode == "power") wire.ignoredWrite = 0x1B;
  if (mode == "read") wire.failedRead = 0x1D;
  if (mode == "write") wire.failedWrite = 0x1D;
  if (mode == "fatal") wire.regs[0x02] = 0x01;
  if (mode == "command") wire.regs[0x02] = 0x02;
  if (mode == "configuration") wire.regs[0x02] = 0x04;
  if (mode == "reserved") {
    wire.overrideRegister = 0x1B;
    wire.overrideValue = 0xB3;
  }
  pet::drivers::Pca9548a mux(wire);
  pet::drivers::Bmp390 bmp(mux, wire, 7);
  assert(bmp.begin());
  const bool expected = mode == "ok" || mode == "reserved" || mode == "stale";
  assert(bmp.startRawSampling25Hz() == expected);
  assert(bmp.configuration().verified == expected);
  assert(bmp.configuration().readbackOk == (mode != "read" && mode != "write"));
  if (mode == "ok") {
    assert(wire.regs[0x1B] == 0x33 && wire.regs[0x1C] == 0);
    assert(wire.regs[0x1D] == 3 && wire.regs[0x1F] == 0);
  }
  if (mode == "stale") {
    wire.failedRead = 0x1D;
    assert(!bmp.startRawSampling25Hz());
    assert(!bmp.configuration().verified && !bmp.configuration().readbackOk);
  }
}
""", encoding="utf-8")
        cls.exe = folder / "bmp_config.exe"
        subprocess.run([
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-I", str(folder), "-I", str(ROOT / "src"), str(harness),
            str(ROOT / "src/drivers/bmp390.cpp"), "-o", str(cls.exe),
        ], check=True, capture_output=True, text=True)

    def test_configuration_acceptance_and_failures(self):
        for mode in ("ok", "osr", "odr", "iir", "power", "read", "write",
                     "fatal", "command", "configuration", "reserved", "stale"):
            with self.subTest(mode=mode):
                subprocess.run([str(self.exe), mode], check=True)
