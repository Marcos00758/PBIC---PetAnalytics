"""Parser for the 79-byte PBIC sensor packet stream."""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterable, Iterator

MAGIC = 0xAA55
PACKET_VERSION = 4
MAGIC_BYTES = struct.pack("<H", MAGIC)
PACKET_STRUCT = struct.Struct("<HIH27h4IB")
PACKET_SIZE = PACKET_STRUCT.size
DEFAULT_CHUNK_SIZE = 64 * 1024
BMP_RAW_INVALID = 0xFFFFFFFF

ACCEL_MPS2_PER_COUNT = (8.0 * 9.80665) / 32767.5
DEFAULT_GYRO_RANGE_DPS = 2000.0
MAG_UT_PER_COUNT = 0.15
BMP390_NVM_LENGTH = 21


@dataclass(frozen=True)
class MagnetometerCalibration:
    hard_iron_ut: tuple[float, ...] = (0.0, 0.0, 0.0)
    soft_iron_matrix: tuple[tuple[float, ...], ...] = (
        (1.0, 0.0, 0.0),
        (0.0, 1.0, 0.0),
        (0.0, 0.0, 1.0),
    )

    def apply(self, raw: tuple[int, ...]) -> tuple[float, ...]:
        centered = tuple(
            value * MAG_UT_PER_COUNT - offset
            for value, offset in zip(raw, self.hard_iron_ut)
        )
        return tuple(
            sum(coefficient * value for coefficient, value in zip(row, centered))
            for row in self.soft_iron_matrix
        )


@dataclass(frozen=True)
class Bmp390Calibration:
    t1: float
    t2: float
    t3: float
    p1: float
    p2: float
    p3: float
    p4: float
    p5: float
    p6: float
    p7: float
    p8: float
    p9: float
    p10: float
    p11: float

    @classmethod
    def from_nvm(cls, nvm: bytes) -> "Bmp390Calibration":
        if len(nvm) != BMP390_NVM_LENGTH:
            raise ValueError(f"BMP390 NVM must contain {BMP390_NVM_LENGTH} bytes")
        t1, t2, t3, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11 = (
            struct.unpack_from("<H", nvm, 0)[0],
            struct.unpack_from("<H", nvm, 2)[0],
            struct.unpack_from("<b", nvm, 4)[0],
            struct.unpack_from("<h", nvm, 5)[0],
            struct.unpack_from("<h", nvm, 7)[0],
            struct.unpack_from("<b", nvm, 9)[0],
            struct.unpack_from("<b", nvm, 10)[0],
            struct.unpack_from("<H", nvm, 11)[0],
            struct.unpack_from("<H", nvm, 13)[0],
            struct.unpack_from("<b", nvm, 15)[0],
            struct.unpack_from("<b", nvm, 16)[0],
            struct.unpack_from("<h", nvm, 17)[0],
            struct.unpack_from("<b", nvm, 19)[0],
            struct.unpack_from("<b", nvm, 20)[0],
        )
        return cls(
            t1=t1 / 2**-8,
            t2=t2 / 2**30,
            t3=t3 / 2**48,
            p1=(p1 - 16384) / 2**20,
            p2=(p2 - 16384) / 2**29,
            p3=p3 / 2**32,
            p4=p4 / 2**37,
            p5=p5 / 2**-3,
            p6=p6 / 2**6,
            p7=p7 / 2**8,
            p8=p8 / 2**15,
            p9=p9 / 2**48,
            p10=p10 / 2**48,
            p11=p11 / 2**65,
        )

    def compensate(self, pressure_raw: int, temperature_raw: int) -> tuple[float, float]:
        if pressure_raw == BMP_RAW_INVALID or temperature_raw == BMP_RAW_INVALID:
            return float("nan"), float("nan")
        temperature_delta = temperature_raw - self.t1
        temperature_c = (
            temperature_delta * self.t2
            + temperature_delta * temperature_delta * self.t3
        )
        pressure_offset = (
            self.p5
            + self.p6 * temperature_c
            + self.p7 * temperature_c**2
            + self.p8 * temperature_c**3
        )
        pressure_sensitivity = pressure_raw * (
            self.p1
            + self.p2 * temperature_c
            + self.p3 * temperature_c**2
            + self.p4 * temperature_c**3
        )
        pressure_nonlinearity = (
            pressure_raw**2 * (self.p9 + self.p10 * temperature_c)
            + pressure_raw**3 * self.p11
        )
        return pressure_offset + pressure_sensitivity + pressure_nonlinearity, temperature_c


@dataclass(frozen=True)
class ImuPacket:
    timestamp_us: int
    sequence: int
    values: tuple[int, ...]
    bmp_raw: tuple[tuple[int, int], tuple[int, int]]

    def physical_values(
        self,
        mag_calibrations: tuple[MagnetometerCalibration, ...] | None = None,
        gyro_range_dps: float = DEFAULT_GYRO_RANGE_DPS,
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
                converted[f"icm{sensor}_g{axis}"] = (
                    value * gyro_range_dps / 32768.0
                )
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
        timestamp_us=unpacked[1],
        sequence=unpacked[2],
        values=unpacked[3:30],
        bmp_raw=((unpacked[30], unpacked[31]), (unpacked[32], unpacked[33])),
    )


def _record_sequence(stats: ParseStats, previous: int | None, current: int) -> int:
    if previous is not None:
        delta = (current - previous) & 0xFFFF
        if 1 < delta < 0x8000:
            stats.sequence_gaps += delta - 1
    return current


def iter_binary_packets(
    stream: BinaryIO,
    stats: ParseStats,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
    max_bytes: int | None = None,
) -> Iterator[ParsedPacket]:
    """Yield valid packets while retaining only a small byte buffer."""
    if chunk_size < PACKET_SIZE:
        raise ValueError(f"chunk_size must be at least {PACKET_SIZE}")

    pending = bytearray()
    absolute_offset = 0
    previous_sequence: int | None = None
    remaining = max_bytes

    while True:
        if remaining is not None and remaining <= 0:
            chunk = b""
        else:
            read_size = chunk_size if remaining is None else min(chunk_size, remaining)
            chunk = stream.read(read_size)
            if remaining is not None:
                remaining -= len(chunk)
        if chunk:
            pending.extend(chunk)

        while len(pending) >= PACKET_SIZE:
            magic_offset = pending.find(MAGIC_BYTES)
            if magic_offset < 0:
                keep = 1 if pending[-1:] == MAGIC_BYTES[:1] else 0
                discarded = len(pending) - keep
                stats.discarded_bytes += discarded
                absolute_offset += discarded
                del pending[:discarded]
                break

            if magic_offset > 0:
                stats.discarded_bytes += magic_offset
                absolute_offset += magic_offset
                del pending[:magic_offset]
            if len(pending) < PACKET_SIZE:
                break

            try:
                packet = decode_packet(pending[:PACKET_SIZE])
            except ValueError:
                stats.crc_failures += 1
                stats.discarded_bytes += 1
                absolute_offset += 1
                del pending[0]
                continue

            parsed = ParsedPacket(offset=absolute_offset, packet=packet)
            previous_sequence = _record_sequence(
                stats, previous_sequence, packet.sequence
            )
            stats.valid_packets += 1
            absolute_offset += PACKET_SIZE
            del pending[:PACKET_SIZE]
            yield parsed

        if not chunk:
            break

    stats.trailing_bytes = len(pending)


def iter_file_packets(
    path: Path,
    stats: ParseStats,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
) -> Iterator[ParsedPacket]:
    journal = load_session_journal(path)
    valid_bytes = _validated_journal_size(path, journal.get("imu_valid_bytes"))
    with path.open("rb") as stream:
        yield from iter_binary_packets(stream, stats, chunk_size, valid_bytes)


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


def load_key_value_file(path: Path) -> dict[str, str]:
    if not path.exists():
        return {}
    values: dict[str, str] = {}
    for line in path.read_text(encoding="ascii", errors="replace").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    return values


def load_session_metadata(input_path: Path) -> dict[str, str]:
    """Load key/value metadata written next to an SD session imu.bin."""
    return load_key_value_file(input_path.parent / "meta.txt")


def load_session_journal(input_path: Path) -> dict[str, str]:
    """Load the last committed sizes for a preallocated SD session."""
    session = input_path if input_path.is_dir() else input_path.parent
    return load_key_value_file(session / "journal.txt")


def _validated_journal_size(path: Path, encoded: str | None) -> int | None:
    if encoded is None:
        return None
    try:
        size = int(encoded)
    except ValueError:
        return None
    file_size = path.stat().st_size
    return size if 0 <= size <= file_size else None


def valid_audio_bytes(path: Path) -> int:
    """Return the committed PCM prefix, excluding any preallocated tail."""
    journal = load_session_journal(path)
    size = _validated_journal_size(path, journal.get("audio_valid_bytes"))
    return path.stat().st_size if size is None else size


def load_bmp390_calibrations(
    input_path: Path,
) -> tuple[Bmp390Calibration | None, Bmp390Calibration | None]:
    metadata = load_session_metadata(input_path)
    calibrations: list[Bmp390Calibration | None] = []
    for sensor in range(2):
        encoded = metadata.get(f"bmp{sensor}_nvm", "")
        valid = metadata.get(f"bmp{sensor}_nvm_valid", "1") == "1"
        if not encoded or not valid:
            calibrations.append(None)
            continue
        try:
            nvm = bytes.fromhex(encoded)
            calibrations.append(Bmp390Calibration.from_nvm(nvm))
        except ValueError:
            calibrations.append(None)
    return calibrations[0], calibrations[1]


def summarize(packets: list[ParsedPacket], stats: ParseStats) -> str:
    first_timestamp = packets[0].packet.timestamp_us if packets else None
    last_timestamp = packets[-1].packet.timestamp_us if packets else None
    return summarize_timestamps(stats, first_timestamp, last_timestamp)


def summarize_timestamps(
    stats: ParseStats,
    first_timestamp: int | None,
    last_timestamp: int | None,
) -> str:
    lines = [
        f"packet_size={PACKET_SIZE}",
        f"valid_packets={stats.valid_packets}",
        f"crc_failures={stats.crc_failures}",
        f"discarded_bytes={stats.discarded_bytes}",
        f"trailing_bytes={stats.trailing_bytes}",
        f"sequence_gaps={stats.sequence_gaps}",
    ]
    if stats.valid_packets >= 2 and first_timestamp is not None and last_timestamp is not None:
        duration_us = elapsed_us(first_timestamp, last_timestamp)
        sample_rate = (stats.valid_packets - 1) * 1_000_000 / duration_us if duration_us else 0
        lines.extend(
            (f"timestamp_span_s={duration_us / 1_000_000:.6f}",
             f"effective_rate_hz={sample_rate:.3f}")
        )
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="raw .bin captured from USB serial")
    args = parser.parse_args()

    stats = ParseStats()
    first_timestamp = None
    last_timestamp = None
    for parsed in iter_file_packets(args.input, stats):
        if first_timestamp is None:
            first_timestamp = parsed.packet.timestamp_us
        last_timestamp = parsed.packet.timestamp_us
    print(summarize_timestamps(stats, first_timestamp, last_timestamp))


if __name__ == "__main__":
    main()
