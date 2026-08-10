import struct
import sys
import tempfile
import unittest
import json
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from analyze_imu import (
    analyze_file,
    calculate_timing,
    format_validation,
    load_gyro_range_dps,
    plot_capture,
)
from parse_data import MAGIC, crc8, parse_stream


def make_packet(timestamp_us: int, sequence: int) -> bytes:
    without_crc = struct.pack(
        "<HIH27h4I", MAGIC, timestamp_us, sequence, *([0] * 27), *([0] * 4)
    )
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

    def test_analyzes_file_incrementally(self):
        raw = b"".join(
            make_packet(timestamp, sequence)
            for timestamp, sequence in ((1_000, 1), (11_000, 2), (21_000, 3))
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.bin"
            path.write_bytes(raw)
            packets, stats, timing, diagnostics = analyze_file(path)
        self.assertEqual(stats.valid_packets, 3)
        self.assertEqual(len(packets), 3)
        self.assertIsNotNone(timing)
        self.assertEqual(diagnostics.accel_near_limit, (0, 0, 0))

    def test_preserves_v3_gyro_scale_from_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.bin"
            metadata = path.with_suffix(".bin.json")
            metadata.write_text(json.dumps({"packet_version": 3}), encoding="utf-8")
            version, gyro_range = load_gyro_range_dps(path)
        self.assertEqual(version, 3)
        self.assertEqual(gyro_range, 1000.0)

    def test_loads_v4_gyro_scale_from_sd_meta(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "imu.bin"
            path.write_bytes(b"")
            (path.parent / "meta.txt").write_text(
                "packet_version=4\ngyro_range_dps=2000\n", encoding="ascii"
            )
            version, gyro_range = load_gyro_range_dps(path)
        self.assertEqual(version, 4)
        self.assertEqual(gyro_range, 2000.0)

    def test_generates_imu_and_magnetometer_pngs(self):
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
            mag_output = Path(directory) / "capture_mag.png"
            bmp_output = Path(directory) / "capture_bmp.png"
            plot_capture(
                packets,
                stats,
                timing,
                output,
                show=False,
                mag_output=mag_output,
                bmp_output=bmp_output,
            )
            self.assertGreater(output.stat().st_size, 0)
            self.assertGreater(mag_output.stat().st_size, 0)
            self.assertGreater(bmp_output.stat().st_size, 0)


if __name__ == "__main__":
    unittest.main()
