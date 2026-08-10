"""Parser for the 63-byte PBIC IMU packet stream."""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

MAGIC = 0xAA55
PACKET_VERSION = 2
MAGIC_BYTES = struct.pack("<H", MAGIC)
PACKET_STRUCT = struct.Struct("<HIH27hB")
PACKET_SIZE = PACKET_STRUCT.size

VALUE_NAMES = tuple(
    f"icm{sensor}_{axis}"
    for sensor in range(3)
    for axis in ("ax", "ay", "az", "gx", "gy", "gz", "mx", "my", "mz")
)

ACCEL_MPS2_PER_COUNT = (2.0 * 9.80665) / 32767.5
GYRO_DPS_PER_COUNT = 250.0 / 32768.0
MAG_UT_PER_COUNT = 0.15


@dataclass(frozen=True)
class MagnetometerCalibration:
    hard_iron_ut: tuple[float, float, float] = (0.0, 0.0, 0.0)
    soft_iron_matrix: tuple[
        tuple[float, float, float],
        tuple[float, float, float],
        tuple[float, float, float],
    ] = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))

    def apply(self, raw: tuple[int, int, int]) -> tuple[float, float, float]:
        centered = tuple(
            value * MAG_UT_PER_COUNT - offset
            for value, offset in zip(raw, self.hard_iron_ut)
        )
        return tuple(
            sum(coefficient * value for coefficient, value in zip(row, centered))
            for row in self.soft_iron_matrix
        )


@dataclass(frozen=True)
class ImuPacket:
    timestamp_us: int
    sequence: int
    values: tuple[int, ...]

    def physical_values(
        self,
        mag_calibrations: tuple[MagnetometerCalibration, ...] | None = None,
    ) -> dict[str, float]:
        converted: dict[str, float] = {}
        calibrations = mag_calibrations or (MagnetometerCalibration(),) * 3
        if len(calibrations) != 3:
            raise ValueError("three magnetometer calibrations are required")

        for sensor in range(3):
            base = sensor * 9
            for axis, value in zip(("x", "y", "z"), self.values[base : base + 3]):
                converted[f"icm{sensor}_a{axis}"] = value * ACCEL_MPS2_PER_COUNT
            for axis, value in zip(("x", "y", "z"), self.values[base + 3 : base + 6]):
                converted[f"icm{sensor}_g{axis}"] = value * GYRO_DPS_PER_COUNT
            magnetic = calibrations[sensor].apply(self.values[base + 6 : base + 9])
            for axis, value in zip(("x", "y", "z"), magnetic):
                converted[f"icm{sensor}_m{axis}"] = value
        return converted


@dataclass(frozen=True)
class ParsedPacket:
    offset: int
    packet: ImuPacket


@dataclass
class ParseStats:
    valid_packets: int = 0
    crc_failures: int = 0
    discarded_bytes: int = 0
    trailing_bytes: int = 0
    sequence_gaps: int = 0


def crc8(data: bytes | bytearray | memoryview) -> int:
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def decode_packet(data: bytes | bytearray | memoryview) -> ImuPacket:
    if len(data) != PACKET_SIZE:
        raise ValueError(f"packet must contain {PACKET_SIZE} bytes")
    if data[:2] != MAGIC_BYTES:
        raise ValueError("invalid packet magic")
    if crc8(data[:-1]) != data[-1]:
        raise ValueError("invalid packet CRC-8")

    unpacked = PACKET_STRUCT.unpack(data)
    return ImuPacket(
        timestamp_us=unpacked[1], sequence=unpacked[2], values=unpacked[3:-1]
    )


def parse_stream(data: bytes | bytearray | memoryview) -> tuple[list[ParsedPacket], ParseStats]:
    raw = bytes(data)
    packets: list[ParsedPacket] = []
    stats = ParseStats()
    offset = 0

    while len(raw) - offset >= PACKET_SIZE:
        magic_offset = raw.find(MAGIC_BYTES, offset)
        if magic_offset < 0:
            stats.discarded_bytes += len(raw) - offset
            offset = len(raw)
            break

        stats.discarded_bytes += magic_offset - offset
        candidate = raw[magic_offset : magic_offset + PACKET_SIZE]
        if len(candidate) < PACKET_SIZE:
            offset = magic_offset
            break

        try:
            packet = decode_packet(candidate)
        except ValueError:
            stats.crc_failures += 1
            stats.discarded_bytes += 1
            offset = magic_offset + 1
            continue

        packets.append(ParsedPacket(offset=magic_offset, packet=packet))
        offset = magic_offset + PACKET_SIZE

    stats.trailing_bytes = len(raw) - offset
    stats.valid_packets = len(packets)
    stats.sequence_gaps = count_sequence_gaps(item.packet for item in packets)
    return packets, stats


def count_sequence_gaps(packets: Iterable[ImuPacket]) -> int:
    iterator = iter(packets)
    previous = next(iterator, None)
    if previous is None:
        return 0

    gaps = 0
    for current in iterator:
        delta = (current.sequence - previous.sequence) & 0xFFFF
        if 1 < delta < 0x8000:
            gaps += delta - 1
        previous = current
    return gaps


def elapsed_us(start_timestamp: int, end_timestamp: int) -> int:
    return (end_timestamp - start_timestamp) & 0xFFFFFFFF


def summarize(packets: list[ParsedPacket], stats: ParseStats) -> str:
    lines = [
        f"packet_size={PACKET_SIZE}",
        f"valid_packets={stats.valid_packets}",
        f"crc_failures={stats.crc_failures}",
        f"discarded_bytes={stats.discarded_bytes}",
        f"trailing_bytes={stats.trailing_bytes}",
        f"sequence_gaps={stats.sequence_gaps}",
    ]
    if len(packets) >= 2:
        duration_us = elapsed_us(
            packets[0].packet.timestamp_us, packets[-1].packet.timestamp_us
        )
        sample_rate = (len(packets) - 1) * 1_000_000 / duration_us if duration_us else 0
        lines.extend(
            (f"timestamp_span_s={duration_us / 1_000_000:.6f}",
             f"effective_rate_hz={sample_rate:.3f}")
        )
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="raw .bin captured from USB serial")
    args = parser.parse_args()

    packets, stats = parse_stream(args.input.read_bytes())
    print(summarize(packets, stats))


if __name__ == "__main__":
    main()
