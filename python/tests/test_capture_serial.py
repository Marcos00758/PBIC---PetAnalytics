import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from capture_serial import capture_sensor_window
from parse_data import MAGIC, PACKET_SIZE, crc8, parse_stream


def make_packet(timestamp_us: int, sequence: int) -> bytes:
    without_crc = struct.pack(
        "<HIH27h4I", MAGIC, timestamp_us, sequence, *([0] * 27), *([0] * 4)
    )
    return without_crc + bytes((crc8(without_crc),))


class FakeSerial:
    def __init__(self, data: bytes, max_read_size: int = 17):
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


class CaptureSerialTest(unittest.TestCase):
    def test_captures_exact_window_across_partial_reads(self):
        stream = b"boot\n" + b"".join(
            make_packet(timestamp, sequence)
            for timestamp, sequence in (
                (1_000, 1),
                (11_000, 2),
                (21_000, 3),
                (31_000, 4),
            )
        )
        raw, first_timestamp = capture_sensor_window(FakeSerial(stream), 0.02)
        packets, stats = parse_stream(raw)
        self.assertEqual(PACKET_SIZE, 79)
        self.assertEqual(first_timestamp, 1_000)
        self.assertEqual([item.packet.sequence for item in packets], [1, 2])
        self.assertEqual(stats.sequence_gaps, 0)


if __name__ == "__main__":
    unittest.main()
