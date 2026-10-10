"""Compile the real acquisition/packet code with injected sensors and clock."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))
from parse_data import parse_stream


class AcquisitionHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("g++ required for C++ acquisition checks")
        cls.build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.build.cleanup)
        cls.exe = Path(cls.build.name) / "acquisition.exe"
        subprocess.run([
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "test/acquisition_host"), "-I", str(ROOT / "src"),
            str(ROOT / "test/acquisition_host.cpp"),
            str(ROOT / "src/services/imu_acquisition.cpp"),
            str(ROOT / "src/utils/packet.cpp"), str(ROOT / "src/utils/crc8.cpp"),
            "-o", str(cls.exe),
        ], check=True, capture_output=True, text=True)

    def run_mode(self, mode):
        output = Path(self.build.name) / f"{mode}.bin"
        subprocess.run([str(self.exe), mode, str(output)], check=True)
        packets, stats = parse_stream(output.read_bytes())
        self.assertEqual(stats.crc_failures, 0)
        self.assertEqual(stats.discarded_bytes, 0)
        self.assertEqual(stats.trailing_bytes, 0)
        return [item.packet for item in packets]

    def test_raw_cpp_packet_is_readable_in_python(self):
        packets = self.run_mode("raw")
        self.assertEqual(len(packets), 1)
        self.assertEqual(packets[0].sequence, 0)
        self.assertEqual(packets[0].timestamp_us, 10000)
        # Independent binary decode also checks integer counts and BMP order.
        import struct
        raw = (Path(self.build.name) / "raw.bin").read_bytes()
        values = struct.unpack_from("<27h", raw, 8)
        self.assertEqual(values[:9], (100, -100, 101, 102, 103, 104, 105, 106, 107))
        self.assertEqual(values[9], 200)
        self.assertEqual(values[18], 300)
        self.assertEqual(struct.unpack_from("<4I", raw, 62),
                         (0x010203, 0x040506, 0x070809, 0x0a0b0c))

    def test_delayed_acquisition_skips_deadlines_without_a_burst(self):
        packets = self.run_mode("late")
        self.assertEqual([p.sequence for p in packets], [2, 3])
        self.assertEqual([p.timestamp_us for p in packets], [35000, 40000])

    def test_sensor_failure_preserves_a_sequence_gap(self):
        packets = self.run_mode("failure")
        self.assertEqual(len(packets), 1)
        self.assertEqual(packets[0].sequence, 1)

    def test_deadline_survives_microsecond_rollover(self):
        packets = self.run_mode("rollover")
        self.assertEqual(len(packets), 1)
        self.assertEqual(packets[0].timestamp_us, 5904)
