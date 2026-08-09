import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from analyze_imu import calculate_timing, format_validation, plot_capture
from parse_data import MAGIC, crc8, parse_stream


def make_packet(timestamp_us: int, sequence: int) -> bytes:
    without_crc = struct.pack("<HIH18h", MAGIC, timestamp_us, sequence, *([0] * 18))
    return without_crc + bytes((crc8(without_crc),))


class AnalyzeImuTest(unittest.TestCase):
    def test_calculates_rate_period_and_jitter(self):
        raw = b"".join(
            make_packet(timestamp, sequence)
            for timestamp, sequence in ((1000, 1), (11000, 2), (22000, 3))
        )
        packets, _ = parse_stream(raw)
        timing = calculate_timing(packets)

        self.assertIsNotNone(timing)
        self.assertAlmostEqual(timing.duration_s, 0.021)
        self.assertAlmostEqual(timing.effective_rate_hz, 2_000_000 / 21_000)
        self.assertAlmostEqual(timing.mean_period_ms, 10.5)
        self.assertAlmostEqual(timing.jitter_std_ms, 0.5)
        self.assertAlmostEqual(timing.min_period_ms, 10.0)
        self.assertAlmostEqual(timing.max_period_ms, 11.0)

    def test_reports_crc_and_sequence_losses(self):
        damaged = bytearray(make_packet(11_000, 2))
        damaged[10] ^= 0x01
        raw = make_packet(1_000, 1) + damaged + make_packet(31_000, 3)
        packets, stats = parse_stream(raw)
        report = format_validation(stats, calculate_timing(packets))

        self.assertIn("crc_failures=1", report)
        self.assertIn("sequence_gaps=1", report)

    def test_generates_six_panel_png(self):
        import matplotlib

        matplotlib.use("Agg")
        raw = b"".join(
            make_packet(timestamp, sequence)
            for timestamp, sequence in ((1_000, 1), (11_000, 2), (21_000, 3))
        )
        packets, stats = parse_stream(raw)
        timing = calculate_timing(packets)
        self.assertIsNotNone(timing)

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.png"
            plot_capture(packets, stats, timing, output, show=False)
            self.assertGreater(output.stat().st_size, 0)


if __name__ == "__main__":
    unittest.main()
