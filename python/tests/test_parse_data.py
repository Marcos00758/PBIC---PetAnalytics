import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from parse_data import MAGIC, PACKET_SIZE, PACKET_STRUCT, crc8, parse_stream


def make_packet(timestamp_us=10_000, sequence=7, values=range(18)):
    without_crc = struct.pack("<HIH18h", MAGIC, timestamp_us, sequence, *values)
    return without_crc + bytes((crc8(without_crc),))


class ParseDataTest(unittest.TestCase):
    def test_packet_contract_is_45_bytes(self):
        self.assertEqual(PACKET_SIZE, 45)
        self.assertEqual(PACKET_STRUCT.size, 45)

    def test_parses_valid_packet(self):
        packets, stats = parse_stream(make_packet())
        self.assertEqual(stats.valid_packets, 1)
        self.assertEqual(stats.crc_failures, 0)
        self.assertEqual(packets[0].packet.sequence, 7)
        self.assertEqual(packets[0].packet.values, tuple(range(18)))

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
