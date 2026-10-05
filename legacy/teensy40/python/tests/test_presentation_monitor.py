import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from parse_data import MAGIC, ParseStats, crc8
from presentation_monitor import (
    DeviceInfo,
    FAIL,
    OK,
    StreamDecoder,
    WARN,
    collect_window,
    evaluate_all,
    evaluate_bmp,
    evaluate_icm,
    SerialSource,
)

BMP_SENTINEL = 0xFFFFFFFF
ACCEL_GRAVITY_COUNTS = 4096
MAG_COUNTS = 300


def make_nvm(pressure_pa: int = 101_320) -> bytes:
    """Build BMP390 trim bytes whose compensation returns a fixed pressure."""
    return struct.pack(
        "<HHbhhbbHHbbhbb",
        0,  # t1
        0,  # t2
        0,  # t3
        16384,  # p1 maps to zero
        16384,  # p2 maps to zero
        0,  # p3
        0,  # p4
        pressure_pa // 8,  # p5 carries the whole offset
        0,  # p6
        0,  # p7
        0,  # p8
        0,  # p9
        0,  # p10
        0,  # p11
    )


def make_packet(
    timestamp_us: int,
    sequence: int,
    values: tuple[int, ...],
    bmp: tuple[int, int, int, int],
) -> bytes:
    without_crc = struct.pack(
        "<HIH27h4I", MAGIC, timestamp_us, sequence, *values, *bmp
    )
    return without_crc + bytes((crc8(without_crc),))


def healthy_values(index: int) -> tuple[int, ...]:
    """Three ICMs at rest: gravity on z, no rotation, a stable magnetic field."""
    values = []
    for sensor in range(3):
        noise = (index + sensor) % 3 - 1
        values.extend((0, 0, ACCEL_GRAVITY_COUNTS + noise))
        values.extend((0, 0, 0))
        magnetic = MAG_COUNTS + (index // 5)
        values.extend((magnetic, 0, 0))
    return tuple(values)


def healthy_bmp(index: int) -> tuple[int, int, int, int]:
    pressure = 8_000_000 + index // 4
    temperature = 6_000_000
    return (pressure, temperature, pressure, temperature)


def healthy_stream(count: int = 100) -> bytes:
    return b"".join(
        make_packet(index * 10_000, index, healthy_values(index), healthy_bmp(index))
        for index in range(count)
    )


BANNER = (
    "PRESENTATION_BANNER firmware_version=0.5.1 packet_version=4 packet_size=79 "
    "imu_sample_rate_hz=100 bmp_sample_rate_hz=25 mag_sample_rate_hz=20 "
    "mag_poll_rate_hz=25 accel_range_g=8 gyro_range_dps=2000 streaming=1\n"
    "PRESENTATION_ICM index=0 channel=4 ready=1 address=0x69 who_am_i=0xEA mag_wia2=0x09\n"
    "PRESENTATION_ICM index=1 channel=5 ready=1 address=0x69 who_am_i=0xEA mag_wia2=0x09\n"
    "PRESENTATION_ICM index=2 channel=6 ready=1 address=0x69 who_am_i=0xEA mag_wia2=0x09\n"
    "PRESENTATION_BMP index=0 channel=7 ready=1 address=0x77 chip_id=0x60\n"
    "PRESENTATION_BMP_NVM index=0 valid=1 nvm={nvm}\n"
    "PRESENTATION_BMP index=1 channel=3 ready=1 address=0x77 chip_id=0x60\n"
    "PRESENTATION_BMP_NVM index=1 valid=1 nvm={nvm}\n"
).format(nvm=make_nvm().hex().upper()).encode("ascii")


def ready_device() -> DeviceInfo:
    device = DeviceInfo()
    for line in BANNER.decode("ascii").splitlines():
        device.update(line)
    return device


class FakeSerial:
    def __init__(self, data: bytes, max_read_size: int = 23):
        self.data = bytearray(data)
        self.max_read_size = max_read_size

    @property
    def in_waiting(self) -> int:
        return len(self.data)

    def read(self, size: int) -> bytes:
        count = min(size, self.max_read_size, len(self.data))
        result = bytes(self.data[:count])
        del self.data[:count]
        return result


class StreamDecoderTest(unittest.TestCase):
    def test_separates_banner_lines_from_packets(self):
        decoder = StreamDecoder()
        stream = BANNER + healthy_stream(10)
        packets = []
        for start in range(0, len(stream), 7):
            packets.extend(decoder.feed(stream[start : start + 7]))

        self.assertEqual(len(packets), 10)
        self.assertTrue(decoder.device.ready)
        self.assertTrue(decoder.device.streaming)
        self.assertEqual(decoder.device.header["firmware_version"], "0.5.1")
        self.assertEqual(decoder.device.icms[2]["channel"], "6")
        self.assertTrue(decoder.device.icm_ready(0))
        self.assertTrue(decoder.device.bmp_ready(1))
        self.assertIsNotNone(decoder.device.calibration(0))
        self.assertEqual(decoder.stats.valid_packets, 10)
        self.assertEqual(decoder.stats.sequence_gaps, 0)

    def test_recovers_alignment_after_corruption(self):
        decoder = StreamDecoder()
        stream = healthy_stream(4)
        corrupted = bytearray(stream)
        corrupted[100] ^= 0xFF
        packets = decoder.feed(bytes(corrupted))

        self.assertGreaterEqual(decoder.stats.crc_failures, 1)
        self.assertGreaterEqual(len(packets), 2)

    def test_reports_sequence_gaps(self):
        decoder = StreamDecoder()
        stream = b"".join(
            make_packet(index * 10_000, sequence, healthy_values(index), healthy_bmp(index))
            for index, sequence in enumerate((0, 1, 5, 6))
        )
        decoder.feed(stream)
        self.assertEqual(decoder.stats.sequence_gaps, 3)

    def test_counts_banner_bytes_apart_from_corruption(self):
        decoder = StreamDecoder()
        decoder.feed(BANNER + healthy_stream(10))

        self.assertEqual(decoder.banner_bytes, len(BANNER))
        self.assertEqual(decoder.stats.discarded_bytes, len(BANNER))

    def test_records_firmware_error_lines(self):
        decoder = StreamDecoder()
        decoder.feed(b"PRESENTATION_ERROR reason=pca9548a_absent\n")
        self.assertEqual(decoder.device.errors, ["pca9548a_absent"])


class EvaluateIcmTest(unittest.TestCase):
    def decode_stream(self, stream: bytes):
        decoder = StreamDecoder()
        return decoder.feed(stream), decoder.stats

    def test_accepts_sensor_at_rest(self):
        packets, _ = self.decode_stream(healthy_stream())
        verdict = evaluate_icm(0, packets, ready_device(), 0.99)

        self.assertEqual(verdict.level, OK, verdict.format())
        self.assertEqual(verdict.reasons, ())
        self.assertAlmostEqual(float(verdict.details["accel_norm_mps2"]), 9.807, places=2)

    def test_rejects_uninitialized_sensor(self):
        device = ready_device()
        device.icms[1]["ready"] = "0"
        packets, _ = self.decode_stream(healthy_stream())
        verdict = evaluate_icm(1, packets, device, 0.99)

        self.assertEqual(verdict.level, FAIL)
        self.assertIn("not_initialized", verdict.reasons)

    def test_detects_frozen_magnetometer(self):
        stream = b"".join(
            make_packet(
                index * 10_000,
                index,
                healthy_values(0),  # every packet repeats the first magnetic sample
                healthy_bmp(index),
            )
            for index in range(100)
        )
        packets, _ = self.decode_stream(stream)
        verdict = evaluate_icm(0, packets, ready_device(), 0.99)

        self.assertEqual(verdict.level, FAIL)
        self.assertIn("magnetometer_frozen", verdict.reasons)

    def test_detects_missing_gravity(self):
        stream = b"".join(
            make_packet(
                index * 10_000,
                index,
                tuple(0 if position % 9 == 2 else value
                      for position, value in enumerate(healthy_values(index))),
                healthy_bmp(index),
            )
            for index in range(100)
        )
        packets, _ = self.decode_stream(stream)
        verdict = evaluate_icm(0, packets, ready_device(), 0.99)

        self.assertEqual(verdict.level, FAIL)
        self.assertIn("gravity_out_of_range", verdict.reasons)

    def test_detects_rotation_during_the_rest_window(self):
        values = list(healthy_values(0))
        values[3] = 2000  # gyro x well above the rest threshold
        stream = b"".join(
            make_packet(index * 10_000, index, tuple(values), healthy_bmp(index))
            for index in range(100)
        )
        packets, _ = self.decode_stream(stream)
        verdict = evaluate_icm(0, packets, ready_device(), 0.99)

        self.assertEqual(verdict.level, FAIL)
        self.assertIn("gyro_not_at_rest", verdict.reasons)


class EvaluateBmpTest(unittest.TestCase):
    def decode_stream(self, stream: bytes):
        decoder = StreamDecoder()
        return decoder.feed(stream)

    def test_compensates_pressure_with_banner_nvm(self):
        packets = self.decode_stream(healthy_stream())
        verdict = evaluate_bmp(0, packets, ready_device(), 0.99)

        self.assertEqual(verdict.level, OK, verdict.format())
        self.assertEqual(verdict.details["compensation"], "bosch")
        self.assertTrue(verdict.details["pressure_hpa"].startswith("1013.2"))

    def test_falls_back_to_raw_counts_without_nvm(self):
        device = ready_device()
        device.nvm[0]["valid"] = "0"
        packets = self.decode_stream(healthy_stream())
        verdict = evaluate_bmp(0, packets, device, 0.99)

        self.assertEqual(verdict.level, WARN)
        self.assertIn("nvm_unavailable", verdict.reasons)
        self.assertEqual(verdict.details["compensation"], "unavailable_raw_only")

    def test_rejects_sentinel_only_cache(self):
        stream = b"".join(
            make_packet(
                index * 10_000,
                index,
                healthy_values(index),
                (BMP_SENTINEL, BMP_SENTINEL, BMP_SENTINEL, BMP_SENTINEL),
            )
            for index in range(100)
        )
        packets = self.decode_stream(stream)
        verdict = evaluate_bmp(0, packets, ready_device(), 0.99)

        self.assertEqual(verdict.level, FAIL)
        self.assertIn("no_valid_samples", verdict.reasons)

    def test_detects_frozen_barometer(self):
        stream = b"".join(
            make_packet(index * 10_000, index, healthy_values(index), healthy_bmp(0))
            for index in range(100)
        )
        packets = self.decode_stream(stream)
        verdict = evaluate_bmp(1, packets, ready_device(), 0.99)

        self.assertEqual(verdict.level, FAIL)
        self.assertIn("barometer_frozen", verdict.reasons)


class EvaluateAllTest(unittest.TestCase):
    def test_healthy_capture_passes_every_check(self):
        decoder = StreamDecoder()
        packets = decoder.feed(BANNER + healthy_stream())
        verdicts, overall = evaluate_all(
            packets, decoder.stats, decoder.device, decoder.banner_bytes
        )

        self.assertEqual(overall, OK, "\n".join(item.format() for item in verdicts))
        self.assertEqual(verdicts[0].details["unexpected_bytes"], "0")
        self.assertEqual([verdict.name for verdict in verdicts],
                         ["stream", "icm0", "icm1", "icm2", "bmp0", "bmp1"])

    def test_missing_stream_fails_every_sensor(self):
        device = ready_device()
        verdicts, overall = evaluate_all([], ParseStats(), device)

        self.assertEqual(overall, FAIL)
        self.assertTrue(all(verdict.level == FAIL for verdict in verdicts))


class CollectWindowTest(unittest.TestCase):
    def test_stops_once_the_sensor_window_is_covered(self):
        decoder = StreamDecoder()
        source = SerialSource(FakeSerial(BANNER + healthy_stream(200)))
        packets = collect_window(source, decoder, 0.5, timeout_s=5.0)

        self.assertEqual(len(packets), 51)
        self.assertTrue(decoder.device.ready)

    def test_times_out_when_the_stream_stops(self):
        decoder = StreamDecoder()
        source = SerialSource(FakeSerial(healthy_stream(5)))
        with self.assertRaises(TimeoutError):
            collect_window(source, decoder, 5.0, timeout_s=0.2)


if __name__ == "__main__":
    unittest.main()
