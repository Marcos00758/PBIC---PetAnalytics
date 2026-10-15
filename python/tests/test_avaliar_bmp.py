import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from avaliacao.avaliar_bmp import analyze_capture
from parse_data import Bmp390Calibration, JOURNAL_MAGIC, JOURNAL_STRUCT, MAGIC, crc8


# Synthetic linear conversion: P=80000+raw/1024 Pa, T=raw/65536 C.
NVM = struct.pack("<HHbhhbbHHbbhbb", 0, 16384, 0, 17408, 16384,
                  0, 0, 10000, 0, 0, 0, 0, 0, 0).hex()
META = "packet_version=4\npacket_size=79\nbmp_sample_rate_hz=25\n"
RAW = (10_240_000, 1_638_400)


def packet(timestamp, sequence, bmp0=RAW, bmp1=RAW):
    body = struct.pack("<HIH27h4I", MAGIC, timestamp, sequence,
                       *([0] * 27), *bmp0, *bmp1)
    return body + bytes((crc8(body),))


class EvaluateBmpTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)

    def capture(self, raw=None, prefix=None, nvms=(NVM, NVM), extra=""):
        if raw is None:
            raw = b"".join(packet(i * 10_000, i) for i in range(3))
        (self.folder / "imu.bin").write_bytes(raw)
        meta = META + "".join(f"bmp{i}_nvm_valid=1\nbmp{i}_nvm={nvm}\n"
                              for i, nvm in enumerate(nvms)) + extra
        (self.folder / "meta.txt").write_text(meta, encoding="utf-8")
        record = JOURNAL_STRUCT.pack(JOURNAL_MAGIC, 1, 1, 1, 100,
                                     len(raw) if prefix is None else prefix, 0, 0)
        (self.folder / "journal.bin").write_bytes(record[:-1] + bytes((crc8(record[:-1]),)))
        return self.folder

    def test_compensation_matches_independent_bosch_c_vectors(self):
        fixture = Path(__file__).parent / "fixtures" / "bmp390_bosch.json"
        for v in json.loads(fixture.read_text())['vectors']:
            with self.subTest(nvm=v['nvm'], pressure=v['pressure_raw']):
                cal = Bmp390Calibration.from_nvm(bytes.fromhex(v['nvm']))
                pressure, temperature = cal.compensate(v['pressure_raw'], v['temperature_raw'])
                self.assertAlmostEqual(pressure, v['pressure_pa'], delta=0.0001)
                self.assertAlmostEqual(temperature, v['temperature_c'], delta=1e-9)

    def test_constant_pressure_cache_and_original_files_preserved(self):
        folder = self.capture()
        original = {p.name: p.read_bytes() for p in folder.iterdir()}
        report, code = analyze_capture(folder)
        self.assertEqual(code, 0)
        self.assertIn("pressure_pa min=90000 max=90000 mean=90000 std=0 slope_per_min=0", report)
        self.assertIn("temperature_c min=25 max=25 mean=25 std=0", report)
        self.assertIn("changes=0 longest_unchanged_observed_s=0.020000", report)
        self.assertIn("reference_pressure_pa=unavailable height_m=unavailable", report)
        self.assertEqual(original, {p.name: p.read_bytes() for p in folder.iterdir()})

    def test_fixed_reference_upward_motion_and_return_to_zero(self):
        raised = (RAW[0] - 3072, RAW[1])  # Known decrease of exactly 3 Pa.
        warm_raised = (raised[0], 35 * 65536)  # Height coefficient must stay at reference T.
        raw = b"".join(packet(i * 1_000_000, i, v, v)
                       for i, v in enumerate((RAW, RAW, raised, warm_raised, RAW, RAW)))
        folder = self.capture(raw)
        report, code = analyze_capture(folder, 2, 4, (0, 2))
        self.assertEqual(code, 0)
        self.assertIn("reference_pressure_pa=90000.000000000", report)
        self.assertIn("delta_pressure_mean_pa=-3.000000", report)
        expected_height = 287.05 * 298.15 / 9.80665 * math.log(90000 / 89997)
        self.assertIn(f"height_m min={expected_height:.9g} max={expected_height:.9g}", report)
        report, code = analyze_capture(folder, 4, 6, (0, 2), rest=True)
        self.assertEqual(code, 0)
        self.assertIn("height_m min=0 max=0 mean=0 std=0 slope_per_min=0", report)
        self.assertIn("trend_interpretation=rest_drift", report)

    def test_pressure_slope_without_removing_drift(self):
        raw = b"".join(packet(i * 1_000_000, i, (RAW[0] + i * 1024, RAW[1])) for i in range(3))
        report, _ = analyze_capture(self.capture(raw), reference_s=(0, 1.1), rest=True)
        self.assertIn("pressure_pa min=90000 max=90002 mean=90001 std=0.816496581 slope_per_min=60", report)
        self.assertIn("no_filters_or_drift_correction", report)

    def test_individual_nvm_changes_comparison_and_reference(self):
        nvm1 = bytearray.fromhex(NVM)
        struct.pack_into("<H", nvm1, 11, 10001)  # Second pressure offset is +8 Pa.
        report, code = analyze_capture(self.capture(nvms=(NVM, nvm1.hex())), reference_s=(0, 0.02))
        self.assertEqual(code, 0)
        self.assertIn("pressure_difference_pa min=-8 max=-8 mean=-8 std=0", report)
        self.assertIn("reference_pressure_pa=90008.000000000", report)
        self.assertEqual(report.count("height_m min=0 max=0 mean=0 std=0"), 2)

    def test_bad_nvm_leaves_one_sensor_raw_only(self):
        for nvm in ("", "XX", "00", "00" * 21, "ff" * 21):
            with self.subTest(nvm=nvm):
                report, code = analyze_capture(self.capture(nvms=(nvm, NVM)), reference_s=(0, 0.02))
                self.assertEqual(code, 0)
                self.assertIn("sensor_data_quality=raw_only", report)
                self.assertIn("valid_raw_pairs=3", report)
                self.assertIn("pressure_pa unavailable", report)
                self.assertIn("reference_pressure_pa=90000.000000000", report)

    def test_nvm_requires_explicit_validity_declaration(self):
        self.capture()
        meta = self.folder / "meta.txt"
        meta.write_text(meta.read_text().replace("bmp0_nvm_valid=1\n", ""))
        report, _ = analyze_capture(self.folder)
        self.assertIn("nvm=nvm_not_declared_valid", report)

    def test_invalid_raw_and_physical_data_are_separate_from_crc(self):
        for raw, field in (((0xFFFFFFFF, RAW[1]), "sentinel_packets=1"),
                           ((0x1000000, RAW[1]), "out_of_24bit_packets=1"),
                           ((RAW[0], 0xFFFFFF), "invalid_physical_packets=1")):
            with self.subTest(raw=raw):
                report, code = analyze_capture(self.capture(packet(0, 0, raw) + packet(10_000, 1)))
                self.assertEqual(code, 1)
                self.assertIn("capture_integrity=passed", report)
                self.assertIn("sensor_data_quality=failed", report)
                self.assertIn(field, report)

    def test_prefix_excludes_uncommitted_tail_and_partial_packet_is_not_corruption(self):
        raw = packet(0, 0) + packet(10_000, 1) + packet(20_000, 2)[:54]
        report, code = analyze_capture(self.capture(raw + bytes(1000), prefix=len(raw)))
        self.assertEqual(code, 0)
        self.assertIn("ignored_unconfirmed_bytes=1000", report)
        self.assertIn("trailing_bytes=54", report)

    def test_crc_failure_anywhere_disables_height_reference(self):
        bad = bytearray(packet(20_000, 2)); bad[10] ^= 1
        raw = packet(0, 0) + packet(10_000, 1) + bad + packet(30_000, 3)
        report, code = analyze_capture(self.capture(raw), 0, 0.02, (0, 0.02))
        self.assertEqual(code, 1)
        self.assertIn("crc_failures=1", report)
        self.assertIn("sequence_gaps=1", report)
        self.assertIn("height_m=unavailable", report)

    def test_normal_rollover_and_ambiguous_timestamps(self):
        raw = packet(0xFFFFF000, 0xFFFF) + packet((0xFFFFF000 + 10_000) & 0xFFFFFFFF, 0)
        report, code = analyze_capture(self.capture(raw))
        self.assertEqual(code, 0)
        self.assertIn("timestamp_span_s=0.010000", report)
        for time, seq in ((0, 1), (0xFFFFFFFF, 1), (10_000, 0), (10_000, 0xFFFF)):
            _, code = analyze_capture(self.capture(packet(0, 0) + packet(time, seq)))
            self.assertEqual(code, 1)

    def test_strict_checkpoint_and_invalid_metadata(self):
        self.capture()
        (self.folder / "journal.bin").write_bytes(bytes(32))
        (self.folder / "journal.txt").write_text("imu_valid_bytes=237\n")
        with self.assertRaisesRegex(ValueError, "checkpoint"):
            analyze_capture(self.folder)
        self.capture(prefix=999)
        with self.assertRaises(ValueError):
            analyze_capture(self.folder)
        self.capture(extra="packet_version=3\n")
        with self.assertRaises(ValueError):
            analyze_capture(self.folder)

    def test_partial_reference_and_empty_selection_do_not_certify_height(self):
        folder = self.capture()
        report, _ = analyze_capture(folder, reference_s=(0, 10))
        self.assertIn("height_m=unavailable", report)
        report, code = analyze_capture(folder, start_s=1)
        self.assertEqual(code, 1)
        self.assertIn("capture_integrity=insufficient", report)
        for start, end in ((-1, None), (0, 0), (float('nan'), None)):
            with self.assertRaises(ValueError):
                analyze_capture(folder, start, end)
        for temperature in (-273.15, float('inf')):
            with self.assertRaises(ValueError):
                analyze_capture(folder, air_temperature_c=temperature)

    def test_bad_reference_packet_disables_only_its_sensor_height(self):
        missing = (0xFFFFFFFF, RAW[1])
        raw = (packet(0, 0) + packet(10_000, 1, missing) + packet(20_000, 2)
               + packet(30_000, 3) + packet(40_000, 4))
        report, code = analyze_capture(self.capture(raw), 0.03, 0.05, (0, 0.03))
        self.assertEqual(code, 0)  # Selected raw data are valid; the bad reference is explicit.
        self.assertIn("reference_valid_packets=2 reference_unusable_packets=1", report)
        self.assertIn("reference_pressure_pa=unavailable height_m=unavailable", report)
        self.assertIn("reference_pressure_pa=90000.000000000", report)

    def test_status_is_a_periodic_snapshot_not_a_final_sensor_verdict(self):
        self.capture()
        (self.folder / "status.txt").write_text("uptime_ms=50\npackets_produced=2\nbmp_i2c_failures_0=3\n")
        report, _ = analyze_capture(self.folder)
        self.assertIn("STATUS_SNAPSHOT uptime_ms=50 packets_produced=2", report)
        self.assertIn("bmp_i2c_failures_0=3", report)
        self.assertIn("snapshot_is_periodic_not_per_packet_or_selected_window", report)

    def test_cli_runs_outside_repository_with_explicit_air_temperature(self):
        script = Path(__file__).resolve().parents[1] / "avaliacao" / "avaliar_bmp.py"
        self.capture()
        command = [sys.executable, "-B", str(script), str(self.folder),
                   "--reference-s", "0", "0.02", "--air-temperature-c", "20"]
        result = subprocess.run(command, cwd=self.folder, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("air_temperature_source=user_supplied", result.stdout)
        self.assertIn("air_temperature_c=20.000000", result.stdout)
        (self.folder / "journal.bin").unlink()
        result = subprocess.run(command, cwd=self.folder, capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
