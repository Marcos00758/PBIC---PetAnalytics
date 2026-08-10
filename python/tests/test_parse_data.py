import struct
import sys
import tempfile
import unittest
from io import BytesIO
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from parse_data import (
    MAGIC,
    PACKET_SIZE,
    PACKET_STRUCT,
    MagnetometerCalibration,
    ParseStats,
    crc8,
    iter_binary_packets,
    load_bmp390_calibrations,
    load_session_metadata,
    parse_stream,
)


def make_packet(
    timestamp_us=10_000,
    sequence=7,
    values=range(27),
    bmp_raw=(1_000_000, 2_000_000, 3_000_000, 4_000_000),
):
    without_crc = struct.pack(
        "<HIH27h4I", MAGIC, timestamp_us, sequence, *values, *bmp_raw
    )
    return without_crc + bytes((crc8(without_crc),))


class ParseDataTest(unittest.TestCase):
    def test_packet_contract_is_79_bytes(self):
        self.assertEqual(PACKET_SIZE, 79)
        self.assertEqual(PACKET_STRUCT.size, 79)

    def test_parses_valid_packet(self):
        packets, stats = parse_stream(make_packet())
        self.assertEqual(stats.valid_packets, 1)
        self.assertEqual(stats.crc_failures, 0)
        self.assertEqual(packets[0].packet.sequence, 7)
        self.assertEqual(packets[0].packet.values, tuple(range(27)))
        self.assertEqual(
            packets[0].packet.bmp_raw,
            ((1_000_000, 2_000_000), (3_000_000, 4_000_000)),
        )

    def test_applies_magnetometer_scale_and_calibration(self):
        packet = parse_stream(make_packet(values=[0] * 6 + [100, 200, -100] + [0] * 18))[0][0].packet
        calibration = MagnetometerCalibration(
            hard_iron_ut=(5.0, 10.0, -5.0),
            soft_iron_matrix=((2.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 0.5)),
        )
        physical = packet.physical_values(
            (calibration, MagnetometerCalibration(), MagnetometerCalibration())
        )
        self.assertEqual(
            (physical["icm0_mx"], physical["icm0_my"], physical["icm0_mz"]),
            (20.0, 20.0, -5.0),
        )

    def test_resynchronizes_after_corruption(self):
        damaged = bytearray(make_packet(sequence=8))
        damaged[12] ^= 0x01
        raw = b"startup text\n" + bytes(damaged) + make_packet(sequence=9)
        packets, stats = parse_stream(raw)
        self.assertEqual([item.packet.sequence for item in packets], [9])
        self.assertGreaterEqual(stats.crc_failures, 1)
        self.assertGreater(stats.discarded_bytes, 0)

    def test_counts_sequence_gaps_with_wraparound(self):
        raw = make_packet(sequence=0xFFFF) + make_packet(sequence=1)
        _, stats = parse_stream(raw)
        self.assertEqual(stats.sequence_gaps, 1)

    def test_streams_packets_across_chunk_boundaries(self):
        raw = b"log" + make_packet(sequence=1) + make_packet(sequence=3) + b"tail"
        stats = ParseStats()
        packets = list(iter_binary_packets(BytesIO(raw), stats, chunk_size=79))
        self.assertEqual([item.packet.sequence for item in packets], [1, 3])
        self.assertEqual(stats.valid_packets, 2)
        self.assertEqual(stats.sequence_gaps, 1)
        self.assertEqual(stats.discarded_bytes, 3)
        self.assertEqual(stats.trailing_bytes, 4)

    def test_loads_bmp390_nvm_from_sd_session_metadata(self):
        nvm = struct.pack(
            "<HHbhhbbHHbbhbb",
            100,
            200,
            1,
            16000,
            16010,
            1,
            1,
            20000,
            1000,
            1,
            1,
            10,
            1,
            1,
        )
        with tempfile.TemporaryDirectory() as directory:
            session = Path(directory)
            input_path = session / "imu.bin"
            input_path.write_bytes(b"")
            (session / "meta.txt").write_text(
                "packet_version=4\n"
                f"bmp0_nvm_valid=1\nbmp0_nvm={nvm.hex()}\n"
                "bmp1_nvm_valid=0\nbmp1_nvm=\n",
                encoding="ascii",
            )
            metadata = load_session_metadata(input_path)
            calibrations = load_bmp390_calibrations(input_path)

        self.assertEqual(metadata["packet_version"], "4")
        self.assertIsNotNone(calibrations[0])
        self.assertIsNone(calibrations[1])
        pressure, temperature = calibrations[0].compensate(6_000_000, 8_000_000)
        self.assertTrue(pressure == pressure)
        self.assertTrue(temperature == temperature)


if __name__ == "__main__":
    unittest.main()
