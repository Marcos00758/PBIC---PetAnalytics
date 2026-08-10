import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from parse_data import (
    MAGIC,
    PACKET_SIZE,
    PACKET_STRUCT,
    MagnetometerCalibration,
    crc8,
    parse_stream,
)


def make_packet(timestamp_us=10_000, sequence=7, values=range(27)):
    without_crc = struct.pack("<HIH27h", MAGIC, timestamp_us, sequence, *values)
    return without_crc + bytes((crc8(without_crc),))


class ParseDataTest(unittest.TestCase):
    def test_packet_contract_is_63_bytes(self):
        self.assertEqual(PACKET_SIZE, 63)
        self.assertEqual(PACKET_STRUCT.size, 63)

    def test_parses_valid_packet(self):
        packets, stats = parse_stream(make_packet())
        self.assertEqual(stats.valid_packets, 1)
        self.assertEqual(stats.crc_failures, 0)
        self.assertEqual(packets[0].packet.sequence, 7)
        self.assertEqual(packets[0].packet.values, tuple(range(27)))

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


if __name__ == "__main__":
    unittest.main()
