import struct
import sys
import tempfile
import unittest
from io import BytesIO
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from parse_data import (
    JOURNAL_MAGIC,
    JOURNAL_STRUCT,
    JOURNAL_VERSION,
    MAGIC,
    PACKET_SIZE,
    PACKET_STRUCT,
    MagnetometerCalibration,
    ParseStats,
    accumulated_elapsed_us,
    crc8,
    iter_binary_packets,
    iter_file_packets,
    load_bmp390_calibrations,
    load_session_journal,
    load_session_metadata,
    parse_stream,
    summarize,
    valid_audio_bytes,
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

    def test_accumulates_multiple_timestamp_rollovers(self):
        timestamps = (0xF0000000, 0x40000000, 0x90000000, 0xE0000000,
                      0x30000000, 0x80000000)
        self.assertEqual(
            accumulated_elapsed_us(timestamps),
            5 * 0x50000000,
        )

        raw = b"".join(
            make_packet(timestamp_us=timestamp, sequence=index)
            for index, timestamp in enumerate(timestamps)
        )
        packets, stats = parse_stream(raw)
        report = summarize(packets, stats)
        self.assertIn("timestamp_span_s=6710.886400", report)

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

    def test_uses_journal_to_ignore_preallocated_tails(self):
        with tempfile.TemporaryDirectory() as directory:
            session = Path(directory)
            imu_path = session / "imu.bin"
            packet = make_packet(sequence=12)
            imu_path.write_bytes(packet + bytes(4096))
            audio_path = session / "audio.raw"
            audio_path.write_bytes(b"\x01\x02\x03\x04" + bytes(1024))
            (session / "journal.txt").write_text(
                f"imu_valid_bytes={len(packet)}\n"
                "audio_valid_bytes=4\n",
                encoding="ascii",
            )

            stats = ParseStats()
            packets = list(iter_file_packets(imu_path, stats))

            self.assertEqual(len(packets), 1)
            self.assertEqual(stats.discarded_bytes, 0)
            self.assertEqual(stats.trailing_bytes, 0)
            self.assertEqual(valid_audio_bytes(audio_path), 4)

    def test_uses_last_valid_binary_journal_record(self):
        def journal_record(sequence: int, imu_bytes: int) -> bytes:
            encoded = JOURNAL_STRUCT.pack(
                JOURNAL_MAGIC,
                JOURNAL_VERSION,
                1,
                sequence,
                1234,
                imu_bytes,
                0,
                0,
            )
            return encoded[:-1] + bytes((crc8(encoded[:-1]),))

        with tempfile.TemporaryDirectory() as directory:
            session = Path(directory)
            packet = make_packet(sequence=12)
            imu_path = session / "imu.bin"
            imu_path.write_bytes(packet * 3 + bytes(4096))
            damaged = bytearray(journal_record(2, len(packet) * 3))
            damaged[8] ^= 0x01
            (session / "journal.bin").write_bytes(
                journal_record(0, len(packet))
                + journal_record(1, len(packet) * 2)
                + bytes(damaged)
                + b"partial"
            )

            stats = ParseStats()
            packets = list(iter_file_packets(imu_path, stats))
            journal = load_session_journal(imu_path)

        self.assertEqual(len(packets), 2)
        self.assertEqual(journal["journal_sequence"], "1")
        self.assertEqual(journal["imu_valid_bytes"], str(len(packet) * 2))


if __name__ == "__main__":
    unittest.main()
