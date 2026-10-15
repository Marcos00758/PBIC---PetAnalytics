import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from avaliacao.avaliar_icm import analyze_capture
from parse_data import JOURNAL_MAGIC, JOURNAL_STRUCT, MAGIC, crc8, load_session_journal


# Same norms, different mounting orientations; magnetometer 1 has a known offset.
VALUES = (0, 0, 4096, 16, 0, 0, 300, 0, 0,
          4096, 0, 0, 0, 16, 0, 0, 400, 0,
          0, 0, -4096, -16, 0, 0, 0, 0, 300)
META = "packet_version=4\npacket_size=79\naccel_range_g=8\ngyro_range_dps=2000\n"


def packet(timestamp, sequence, values=VALUES):
    body = struct.pack("<HIH27h4I", MAGIC, timestamp, sequence, *values, 0, 0, 0, 0)
    return body + bytes((crc8(body),))


class EvaluateIcmTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)

    def capture(self, raw=None, prefix=None, metadata=META):
        if raw is None:
            raw = b"".join(packet(i * 10_000, i) for i in range(3))
        (self.folder / "imu.bin").write_bytes(raw)
        (self.folder / "meta.txt").write_text(metadata, encoding="utf-8")
        record = JOURNAL_STRUCT.pack(JOURNAL_MAGIC, 1, 1, 1, 100,
                                     len(raw) if prefix is None else prefix, 0, 0)
        (self.folder / "journal.bin").write_bytes(record[:-1] + bytes((crc8(record[:-1]),)))
        return self.folder

    def test_known_vectors_units_and_orientation_independent_comparison(self):
        folder = self.capture()
        original = {p.name: p.read_bytes() for p in folder.iterdir()}
        report, code = analyze_capture(folder)
        self.assertEqual(code, 0)
        self.assertIn("effective_rate_hz=100.000", report)
        self.assertIn("accel_norm_g min=1 max=1 mean=1 std=0", report)
        self.assertIn("gyro_x raw_counts min=16 max=16 mean=16 std=0 | dps min=0.976562", report)
        self.assertIn("icm0_icm1 accel_norm_abs_difference_g min=0 max=0 mean=0 std=0", report)
        self.assertIn("icm0_icm1 mag_norm_abs_difference_ut min=15 max=15 mean=15 std=0", report)
        self.assertIn("physical_suitability=inconclusive", report)
        self.assertEqual(original, {p.name: p.read_bytes() for p in folder.iterdir()})

    def test_population_deviation_and_declared_rest_window(self):
        raw = b"".join(packet(i * 10_000, i, (x, *VALUES[1:]))
                       for i, x in enumerate((-4096, 4096, 0)))
        report, code = analyze_capture(self.capture(raw), end_s=0.02, rest=True)
        self.assertEqual(code, 0)
        self.assertIn("selected_packets=2 selected_span_s=0.010000", report)
        self.assertIn("accel_x raw_counts min=-4096 max=4096 mean=0 std=4096 | g min=-1 max=1 mean=0 std=1", report)
        self.assertIn("gyro_mean_interpretation=rest_offset", report)

    def test_cached_magnetometer_is_measured_without_asserting_failure(self):
        report, code = analyze_capture(self.capture())
        self.assertEqual(code, 0)
        self.assertIn("mag_norm_ut min=45 max=45 mean=45 std=0 changes=0 longest_unchanged_observed_s=0.020000", report)
        self.assertIn("repetitions_are_not_proof_of_stale_data", report)
        changed = list(VALUES)
        changed[6:9] = [0, 0, 0]
        raw = packet(0, 0) + packet(10_000, 1, changed) + packet(20_000, 2, changed)
        report, _ = analyze_capture(self.capture(raw))
        self.assertIn("mag_zero_packets=2", report)
        self.assertIn("mean=15 std=21.2132 changes=1 longest_unchanged_observed_s=0.010000", report)

    def test_near_limit_samples_in_both_signed_directions(self):
        values = list(VALUES)
        values[:6] = (32000, -32768, 0, 32767, -32000, 0)
        raw = packet(0, 0, values) + packet(10_000, 1, values)
        report, _ = analyze_capture(self.capture(raw))
        self.assertIn("near_limit_axis_samples accel=4 gyro=4 raw_threshold=32000", report)

    def test_confirmed_prefix_and_partial_tail(self):
        good = packet(0, 0) + packet(10_000, 1) + packet(20_000, 2)[:54]
        report, code = analyze_capture(self.capture(good + b"not committed", prefix=len(good)))
        self.assertEqual(code, 0)
        self.assertIn("ignored_unconfirmed_bytes=13", report)
        self.assertIn("trailing_bytes=54", report)

    def test_crc_failure_is_reported_even_outside_selected_window(self):
        bad = bytearray(packet(20_000, 2))
        bad[10] ^= 1
        raw = packet(0, 0) + packet(10_000, 1) + bad + packet(30_000, 3)
        report, code = analyze_capture(self.capture(raw), end_s=0.02)
        self.assertEqual(code, 1)
        self.assertIn("crc_failures=1", report)
        self.assertIn("sequence_gaps=1", report)
        self.assertIn("capture_integrity=failed scope=full_confirmed_prefix", report)

    def test_duplicate_ambiguous_sequence_and_bad_timestamps(self):
        for timestamp, sequence, field in (
            (10_000, 0, "sequence_duplicate_or_ambiguous=1"),
            (10_000, 0xFFFF, "sequence_duplicate_or_ambiguous=1"),
            (0, 1, "timestamp_inconsistencies=1"),
            (0xFFFFFFFF, 1, "timestamp_inconsistencies=1"),
            (10_000, 2, "sequence_gaps=1"),
        ):
            with self.subTest(timestamp=timestamp, sequence=sequence):
                report, code = analyze_capture(self.capture(packet(0, 0) + packet(timestamp, sequence)))
                self.assertEqual(code, 1)
                self.assertIn(field, report)

    def test_normal_sequence_and_timestamp_rollover(self):
        raw = packet(0xFFFFF000, 0xFFFF) + packet((0xFFFFF000 + 10_000) & 0xFFFFFFFF, 0)
        report, code = analyze_capture(self.capture(raw))
        self.assertEqual(code, 0)
        self.assertIn("timestamp_span_s=0.010000", report)

    def test_strict_journal_keeps_legacy_fallback_available_to_other_tools(self):
        self.capture()
        (self.folder / "journal.bin").write_bytes(bytes(JOURNAL_STRUCT.size))
        (self.folder / "journal.txt").write_text("imu_valid_bytes=237\n")
        self.assertEqual(load_session_journal(self.folder)["imu_valid_bytes"], "237")
        with self.assertRaisesRegex(ValueError, "checkpoint"):
            analyze_capture(self.folder)

    def test_invalid_prefix_and_metadata_are_rejected(self):
        for prefix, metadata in ((999, META), (None, META.replace("version=4", "version=3")),
                                 (None, META + "icm1_accel_config=0x06\n")):
            with self.subTest(prefix=prefix, metadata=metadata):
                with self.assertRaises(ValueError):
                    analyze_capture(self.capture(prefix=prefix, metadata=metadata))

    def test_missing_range_stays_raw_and_does_not_assume_eight_g(self):
        report, code = analyze_capture(self.capture(metadata=META.replace("accel_range_g=8\n", "")))
        self.assertEqual(code, 0)
        self.assertIn("| g unavailable", report)
        self.assertIn("physical_scales=incomplete", report)

    def test_snapshot_counters_are_identified_as_periodic(self):
        self.capture()
        (self.folder / "status.txt").write_text("uptime_ms=50\npackets_produced=2\nmag_overflows_0=3\n")
        report, _ = analyze_capture(self.folder)
        self.assertIn("STATUS_SNAPSHOT uptime_ms=50 packets_produced=2", report)
        self.assertIn("mag_overflows_0=3", report)
        self.assertIn("snapshot_is_periodic_not_per_packet_or_selected_window", report)

    def test_insufficient_samples_and_invalid_window(self):
        report, code = analyze_capture(self.capture(b""))
        self.assertEqual(code, 1)
        self.assertIn("capture_integrity=insufficient", report)
        for start, end in ((-1, None), (0, 0), (float("nan"), None), (0, float("inf"))):
            with self.assertRaises(ValueError):
                analyze_capture(self.folder, start_s=start, end_s=end)

    def test_cli_runs_outside_repository_and_rejects_bad_input(self):
        script = Path(__file__).resolve().parents[1] / "avaliacao" / "avaliar_icm.py"
        self.capture()
        command = [sys.executable, "-B", str(script), str(self.folder)]
        result = subprocess.run(command, cwd=self.folder, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("evaluation=icm", result.stdout)
        (self.folder / "journal.bin").unlink()
        result = subprocess.run(command, cwd=self.folder, capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("checkpoint", result.stderr)


if __name__ == "__main__":
    unittest.main()
