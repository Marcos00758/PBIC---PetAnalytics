"""Verify the PBIC sensors from the live USB presentation stream."""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from capture_serial import READ_TIMEOUT_SECONDS, SERIAL_BAUD, detect_port
from parse_data import (
    BMP_RAW_INVALID,
    Bmp390Calibration,
    DEFAULT_GYRO_RANGE_DPS,
    MAGIC_BYTES,
    MAG_UT_PER_COUNT,
    PACKET_SIZE,
    ImuPacket,
    ParseStats,
    decode_packet,
    elapsed_us,
)

try:
    import serial
except ImportError as exc:  # pragma: no cover - mirrors capture_serial.py
    raise SystemExit(
        "pyserial is required; install it with: pip install -r python/requirements.txt"
    ) from exc

BANNER_HEADER = "PRESENTATION_BANNER"
BANNER_ICM = "PRESENTATION_ICM"
BANNER_BMP = "PRESENTATION_BMP"
BANNER_BMP_NVM = "PRESENTATION_BMP_NVM"
BANNER_SCAN = "PRESENTATION_I2C_SCAN"
BANNER_ERROR = "PRESENTATION_ERROR"

DEFAULT_DURATION_SECONDS = 5.0
BANNER_TIMEOUT_SECONDS = 15.0
COLLECT_TIMEOUT_MARGIN_SECONDS = 10.0
MAX_TEXT_BUFFER_BYTES = 4096

GRAVITY_MPS2 = 9.80665
ACCEL_ERROR_OK_MPS2 = 0.5
ACCEL_ERROR_WARN_MPS2 = 1.5
GYRO_REST_OK_DPS = 3.0
GYRO_REST_WARN_DPS = 10.0
MAG_NORM_MIN_UT = 20.0
MAG_NORM_MAX_UT = 80.0
MAG_CHANGE_RATIO_WARN = 0.5
RAW_SATURATION_THRESHOLD = 32_000
PRESSURE_MIN_HPA = 850.0
PRESSURE_MAX_HPA = 1085.0
BMP_CHANGE_RATIO_WARN = 0.5
RATE_TOLERANCE_RATIO = 0.02

OK = "ok"
WARN = "warning"
FAIL = "fail"
LEVEL_ORDER = {OK: 0, WARN: 1, FAIL: 2}


def worst_level(levels: tuple[str, ...]) -> str:
    return max(levels, key=lambda level: LEVEL_ORDER[level], default=OK)


@dataclass
class DeviceInfo:
    """Firmware banner emitted by the presentation mode, parsed as key/value."""

    header: dict[str, str] = field(default_factory=dict)
    icms: dict[int, dict[str, str]] = field(default_factory=dict)
    bmps: dict[int, dict[str, str]] = field(default_factory=dict)
    nvm: dict[int, dict[str, str]] = field(default_factory=dict)
    scan: dict[int, str] = field(default_factory=dict)
    errors: list[str] = field(default_factory=list)

    @property
    def ready(self) -> bool:
        return bool(self.header)

    @property
    def streaming(self) -> bool:
        return self.header.get("streaming") == "1"

    @property
    def accel_range_g(self) -> float:
        return float(self.header.get("accel_range_g", 8))

    @property
    def gyro_range_dps(self) -> float:
        return float(self.header.get("gyro_range_dps", DEFAULT_GYRO_RANGE_DPS))

    @property
    def imu_rate_hz(self) -> float:
        return float(self.header.get("imu_sample_rate_hz", 100))

    @property
    def mag_rate_hz(self) -> float:
        return float(self.header.get("mag_sample_rate_hz", 20))

    @property
    def bmp_rate_hz(self) -> float:
        return float(self.header.get("bmp_sample_rate_hz", 25))

    @property
    def accel_scale_mps2(self) -> float:
        return (self.accel_range_g * GRAVITY_MPS2) / 32767.5

    @property
    def gyro_scale_dps(self) -> float:
        return self.gyro_range_dps / 32768.0

    def icm_ready(self, index: int) -> bool:
        return self.icms.get(index, {}).get("ready") == "1"

    def bmp_ready(self, index: int) -> bool:
        return self.bmps.get(index, {}).get("ready") == "1"

    def calibration(self, index: int) -> Bmp390Calibration | None:
        entry = self.nvm.get(index, {})
        if entry.get("valid") != "1":
            return None
        try:
            return Bmp390Calibration.from_nvm(bytes.fromhex(entry.get("nvm", "")))
        except ValueError:
            return None

    def update(self, line: str) -> bool:
        """Absorb one banner line and report whether it was recognized."""
        if not line:
            return False
        tokens = line.split()
        label = tokens[0]
        values = _key_values(tokens[1:])
        if label == BANNER_HEADER:
            self.header = values
        elif label == BANNER_ICM and "index" in values:
            self.icms[int(values["index"])] = values
        elif label == BANNER_BMP_NVM and "index" in values:
            self.nvm[int(values["index"])] = values
        elif label == BANNER_BMP and "index" in values:
            self.bmps[int(values["index"])] = values
        elif label == BANNER_SCAN and "channel" in values:
            self.scan[int(values["channel"])] = values.get("addresses", "")
        elif label == BANNER_ERROR:
            reason = values.get("reason", "unknown")
            if reason not in self.errors:
                self.errors.append(reason)
        else:
            return False
        return True


def _key_values(tokens: list[str]) -> dict[str, str]:
    values: dict[str, str] = {}
    for token in tokens:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        values[key] = value
    return values


class StreamDecoder:
    """Split the USB stream into v4 packets and firmware banner lines."""

    def __init__(self) -> None:
        self._pending = bytearray()
        self._text = bytearray()
        self._previous_sequence: int | None = None
        self.stats = ParseStats()
        self.device = DeviceInfo()
        self.banner_bytes = 0

    def feed(self, chunk: bytes) -> list[ImuPacket]:
        packets: list[ImuPacket] = []
        self._pending.extend(chunk)

        while len(self._pending) >= PACKET_SIZE:
            index = self._pending.find(MAGIC_BYTES)
            if index < 0:
                keep = 1 if self._pending[-1:] == MAGIC_BYTES[:1] else 0
                self._discard(len(self._pending) - keep)
                break
            if index > 0:
                self._discard(index)
            if len(self._pending) < PACKET_SIZE:
                break

            try:
                packet = decode_packet(self._pending[:PACKET_SIZE])
            except ValueError:
                self.stats.crc_failures += 1
                self._discard(1)
                continue

            del self._pending[:PACKET_SIZE]
            self.stats.valid_packets += 1
            self._record_sequence(packet.sequence)
            packets.append(packet)

        self._flush_short_text()
        return packets

    def _flush_short_text(self) -> None:
        """Release banner lines shorter than a packet, ahead of any magic."""
        while True:
            magic_index = self._pending.find(MAGIC_BYTES)
            limit = len(self._pending) if magic_index < 0 else magic_index
            newline = self._pending.find(b"\n", 0, limit)
            if newline < 0:
                return
            self._discard(newline + 1)

    def _discard(self, count: int) -> None:
        if count <= 0:
            return
        chunk = bytes(self._pending[:count])
        del self._pending[:count]
        self.stats.discarded_bytes += count
        self._consume_text(chunk)

    def _consume_text(self, chunk: bytes) -> None:
        self._text.extend(chunk)
        while b"\n" in self._text:
            line, _, rest = self._text.partition(b"\n")
            self._text = bytearray(rest)
            if self.device.update(line.decode("ascii", errors="replace").strip()):
                self.banner_bytes += len(line) + 1
        if len(self._text) > MAX_TEXT_BUFFER_BYTES:
            del self._text[:-MAX_TEXT_BUFFER_BYTES]

    def _record_sequence(self, sequence: int) -> None:
        if self._previous_sequence is not None:
            delta = (sequence - self._previous_sequence) & 0xFFFF
            if 1 < delta < 0x8000:
                self.stats.sequence_gaps += delta - 1
        self._previous_sequence = sequence


@dataclass(frozen=True)
class StreamHealth:
    duration_s: float
    effective_rate_hz: float
    jitter_std_ms: float
    max_period_ms: float


@dataclass(frozen=True)
class Verdict:
    name: str
    level: str
    details: dict[str, str]
    reasons: tuple[str, ...]

    def format(self) -> str:
        fields = " ".join(f"{key}={value}" for key, value in self.details.items())
        line = f"{self.name} verdict={self.level}"
        if fields:
            line = f"{line} {fields}"
        if self.reasons:
            line = f"{line} reasons={','.join(self.reasons)}"
        return line


def calculate_stream_health(packets: list[ImuPacket]) -> StreamHealth | None:
    if len(packets) < 2:
        return None
    intervals_us = [
        elapsed_us(previous.timestamp_us, current.timestamp_us)
        for previous, current in zip(packets, packets[1:])
    ]
    duration_us = sum(intervals_us)
    rate = len(intervals_us) * 1_000_000 / duration_us if duration_us else math.inf
    return StreamHealth(
        duration_s=duration_us / 1_000_000,
        effective_rate_hz=rate,
        jitter_std_ms=statistics.pstdev(intervals_us) / 1_000,
        max_period_ms=max(intervals_us) / 1_000,
    )


def evaluate_stream(
    stats: ParseStats,
    health: StreamHealth | None,
    device: DeviceInfo,
    banner_bytes: int = 0,
) -> Verdict:
    reasons: list[str] = []
    level = OK
    unexpected_bytes = max(stats.discarded_bytes - banner_bytes, 0)
    details: dict[str, str] = {
        "valid_packets": str(stats.valid_packets),
        "crc_failures": str(stats.crc_failures),
        "sequence_gaps": str(stats.sequence_gaps),
        "banner_bytes": str(banner_bytes),
        "unexpected_bytes": str(unexpected_bytes),
    }

    if health is None:
        return Verdict("stream", FAIL, details, ("no_packets",))

    details["duration_s"] = f"{health.duration_s:.3f}"
    details["effective_rate_hz"] = f"{health.effective_rate_hz:.3f}"
    details["jitter_std_ms"] = f"{health.jitter_std_ms:.3f}"
    details["max_period_ms"] = f"{health.max_period_ms:.3f}"

    expected = device.imu_rate_hz
    if expected > 0:
        deviation = abs(health.effective_rate_hz - expected) / expected
        if deviation > RATE_TOLERANCE_RATIO:
            level = worst_level((level, WARN))
            reasons.append("rate_off_nominal")
    if stats.crc_failures:
        level = worst_level((level, WARN))
        reasons.append("crc_failures")
    if stats.sequence_gaps:
        level = worst_level((level, WARN))
        reasons.append("sequence_gaps")
    if unexpected_bytes:
        level = worst_level((level, WARN))
        reasons.append("unexpected_bytes")
    return Verdict("stream", level, details, tuple(reasons))


def evaluate_icm(
    index: int,
    packets: list[ImuPacket],
    device: DeviceInfo,
    duration_s: float,
    at_rest: bool = True,
) -> Verdict:
    """Grade one ICM. The gravity and rest checks only apply while still."""
    base = index * 9
    reasons: list[str] = []
    level = OK
    details: dict[str, str] = {
        "channel": device.icms.get(index, {}).get("channel", "?")
    }

    if not device.icm_ready(index):
        banner = device.icms.get(index, {})
        details["address"] = banner.get("address", "?")
        details["who_am_i"] = banner.get("who_am_i", "?")
        details["mag_wia2"] = banner.get("mag_wia2", "?")
        return Verdict(f"icm{index}", FAIL, details, ("not_initialized",))
    if not packets:
        return Verdict(f"icm{index}", FAIL, details, ("no_packets",))

    accel_scale = device.accel_scale_mps2
    gyro_scale = device.gyro_scale_dps
    accel_norms: list[float] = []
    gyro_peak = 0.0
    accel_near_limit = 0
    gyro_near_limit = 0
    mag_changes = 0
    mag_norms: list[float] = []
    previous_mag: tuple[int, ...] | None = None
    frozen_accel = True
    first_accel = packets[0].values[base : base + 3]

    for packet in packets:
        accel = packet.values[base : base + 3]
        gyro = packet.values[base + 3 : base + 6]
        mag = packet.values[base + 6 : base + 9]
        if accel != first_accel:
            frozen_accel = False
        accel_norms.append(
            math.sqrt(sum((value * accel_scale) ** 2 for value in accel))
        )
        gyro_peak = max(gyro_peak, max(abs(value * gyro_scale) for value in gyro))
        accel_near_limit += sum(
            1 for value in accel if abs(value) >= RAW_SATURATION_THRESHOLD
        )
        gyro_near_limit += sum(
            1 for value in gyro if abs(value) >= RAW_SATURATION_THRESHOLD
        )
        if previous_mag is not None and mag != previous_mag:
            mag_changes += 1
        previous_mag = mag
        mag_norms.append(
            math.sqrt(sum((value * MAG_UT_PER_COUNT) ** 2 for value in mag))
        )

    accel_norm = statistics.fmean(accel_norms)
    accel_error = abs(accel_norm - GRAVITY_MPS2)
    expected_mag_changes = device.mag_rate_hz * duration_s
    mag_ratio = mag_changes / expected_mag_changes if expected_mag_changes else 0.0

    details.update(
        {
            "accel_norm_mps2": f"{accel_norm:.3f}",
            "accel_error_mps2": f"{accel_error:.3f}",
            "gyro_peak_dps": f"{gyro_peak:.2f}",
            "mag_changes": str(mag_changes),
            "mag_change_ratio": f"{mag_ratio:.2f}",
            "mag_norm_ut": f"{min(mag_norms):.2f}..{max(mag_norms):.2f}",
            "accel_near_limit": str(accel_near_limit),
            "gyro_near_limit": str(gyro_near_limit),
        }
    )

    if frozen_accel:
        level = worst_level((level, FAIL))
        reasons.append("accelerometer_frozen")
    if at_rest:
        if accel_error > ACCEL_ERROR_WARN_MPS2:
            level = worst_level((level, FAIL))
            reasons.append("gravity_out_of_range")
        elif accel_error > ACCEL_ERROR_OK_MPS2:
            level = worst_level((level, WARN))
            reasons.append("gravity_off_nominal")
        if gyro_peak > GYRO_REST_WARN_DPS:
            level = worst_level((level, FAIL))
            reasons.append("gyro_not_at_rest")
        elif gyro_peak > GYRO_REST_OK_DPS:
            level = worst_level((level, WARN))
            reasons.append("gyro_noisy_at_rest")
    if mag_changes == 0:
        level = worst_level((level, FAIL))
        reasons.append("magnetometer_frozen")
    elif mag_ratio < MAG_CHANGE_RATIO_WARN:
        level = worst_level((level, WARN))
        reasons.append("magnetometer_slow_updates")
    if not MAG_NORM_MIN_UT <= statistics.fmean(mag_norms) <= MAG_NORM_MAX_UT:
        level = worst_level((level, WARN))
        reasons.append("magnetic_norm_out_of_range")
    if accel_near_limit or gyro_near_limit:
        level = worst_level((level, WARN))
        reasons.append("near_full_scale")
    return Verdict(f"icm{index}", level, details, tuple(reasons))


def evaluate_bmp(
    index: int, packets: list[ImuPacket], device: DeviceInfo, duration_s: float
) -> Verdict:
    reasons: list[str] = []
    level = OK
    details: dict[str, str] = {
        "channel": device.bmps.get(index, {}).get("channel", "?")
    }

    if not device.bmp_ready(index):
        banner = device.bmps.get(index, {})
        details["address"] = banner.get("address", "?")
        details["chip_id"] = banner.get("chip_id", "?")
        channel = banner.get("channel")
        if channel is not None and channel.isdigit():
            found = device.scan.get(int(channel), "")
            details["scan_on_channel"] = found if found else "none"
        return Verdict(f"bmp{index}", FAIL, details, ("not_initialized",))
    if not packets:
        return Verdict(f"bmp{index}", FAIL, details, ("no_packets",))

    calibration = device.calibration(index)
    invalid = 0
    changes = 0
    previous: tuple[int, int] | None = None
    pressures: list[int] = []
    temperatures: list[int] = []

    for packet in packets:
        pressure_raw, temperature_raw = packet.bmp_raw[index]
        if pressure_raw == BMP_RAW_INVALID or temperature_raw == BMP_RAW_INVALID:
            invalid += 1
            continue
        if previous is not None and (pressure_raw, temperature_raw) != previous:
            changes += 1
        previous = (pressure_raw, temperature_raw)
        pressures.append(pressure_raw)
        temperatures.append(temperature_raw)

    details["invalid_packets"] = str(invalid)
    details["changes"] = str(changes)
    if not pressures:
        return Verdict(f"bmp{index}", FAIL, details, ("no_valid_samples",))

    expected_changes = device.bmp_rate_hz * duration_s
    change_ratio = changes / expected_changes if expected_changes else 0.0
    details["change_ratio"] = f"{change_ratio:.2f}"

    if calibration is None:
        details["compensation"] = "unavailable_raw_only"
        details["pressure_raw"] = f"{min(pressures)}..{max(pressures)}"
        details["temperature_raw"] = f"{min(temperatures)}..{max(temperatures)}"
        level = worst_level((level, WARN))
        reasons.append("nvm_unavailable")
    else:
        compensated = [
            calibration.compensate(pressure, temperature)
            for pressure, temperature in zip(pressures, temperatures)
        ]
        pressures_hpa = [pressure / 100.0 for pressure, _ in compensated]
        celsius = [temperature for _, temperature in compensated]
        details["compensation"] = "bosch"
        details["pressure_hpa"] = (
            f"{min(pressures_hpa):.2f}..{max(pressures_hpa):.2f}"
        )
        details["temperature_c"] = f"{min(celsius):.2f}..{max(celsius):.2f}"
        mean_pressure = statistics.fmean(pressures_hpa)
        if not PRESSURE_MIN_HPA <= mean_pressure <= PRESSURE_MAX_HPA:
            level = worst_level((level, FAIL))
            reasons.append("pressure_out_of_range")

    if invalid:
        level = worst_level((level, WARN))
        reasons.append("invalid_samples")
    if changes == 0:
        level = worst_level((level, FAIL))
        reasons.append("barometer_frozen")
    elif change_ratio < BMP_CHANGE_RATIO_WARN:
        level = worst_level((level, WARN))
        reasons.append("slow_updates")
    return Verdict(f"bmp{index}", level, details, tuple(reasons))


def evaluate_all(
    packets: list[ImuPacket],
    stats: ParseStats,
    device: DeviceInfo,
    banner_bytes: int = 0,
    at_rest: bool = True,
) -> tuple[list[Verdict], str]:
    health = calculate_stream_health(packets)
    duration_s = health.duration_s if health is not None else 0.0
    verdicts = [evaluate_stream(stats, health, device, banner_bytes)]
    verdicts.extend(
        evaluate_icm(index, packets, device, duration_s, at_rest)
        for index in range(3)
    )
    verdicts.extend(
        evaluate_bmp(index, packets, device, duration_s) for index in range(2)
    )
    overall = worst_level(tuple(verdict.level for verdict in verdicts))
    return verdicts, overall


def format_report(device: DeviceInfo, verdicts: list[Verdict], overall: str) -> str:
    lines = [
        f"firmware_version={device.header.get('firmware_version', 'unknown')}",
        f"packet_version={device.header.get('packet_version', 'unknown')}",
        f"packet_size={PACKET_SIZE}",
        f"accel_range_g={device.accel_range_g:g}",
        f"gyro_range_dps={device.gyro_range_dps:g}",
    ]
    if device.errors:
        lines.append(f"firmware_errors={','.join(device.errors)}")
    lines.extend(verdict.format() for verdict in verdicts)
    if overall != OK and device.scan:
        for channel in sorted(device.scan):
            found = device.scan[channel] or "none"
            lines.append(f"i2c_scan channel={channel} addresses={found}")
    lines.append(f"overall_verdict={overall}")
    return "\n".join(lines)


def report_document(
    device: DeviceInfo, verdicts: list[Verdict], overall: str
) -> dict[str, object]:
    return {
        "firmware": device.header,
        "firmware_errors": device.errors,
        "verdicts": [
            {
                "name": verdict.name,
                "level": verdict.level,
                "details": verdict.details,
                "reasons": list(verdict.reasons),
            }
            for verdict in verdicts
        ],
        "overall_verdict": overall,
    }


class SerialSource:
    """Minimal non-blocking reader shared by the CLI and the tests."""

    def __init__(self, port, block: bool = True) -> None:
        self._port = port
        self._block = block

    def read(self) -> bytes:
        waiting = self._port.in_waiting
        if not self._block:
            # The live panel must never stall its redraw waiting for bytes.
            return self._port.read(waiting) if waiting else b""
        return self._port.read(max(waiting, 1))


def wait_for_banner(
    source: SerialSource,
    decoder: StreamDecoder,
    timeout_s: float,
    collected: list[ImuPacket] | None = None,
) -> bool:
    """Read until the firmware identifies itself.

    Packets already arriving alongside the banner are handed back through
    `collected` when a caller asks for them; discarding them silently would
    lose the opening of a capture.
    """
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        packets = decoder.feed(source.read())
        if collected is not None:
            collected.extend(packets)
        if decoder.device.ready:
            return True
    return False


def collect_window(
    source: SerialSource,
    decoder: StreamDecoder,
    duration_s: float,
    timeout_s: float,
) -> list[ImuPacket]:
    collected: list[ImuPacket] = []
    sensor_elapsed_us = 0
    duration_us = round(duration_s * 1_000_000)
    deadline = time.monotonic() + timeout_s

    while time.monotonic() < deadline:
        for packet in decoder.feed(source.read()):
            if collected:
                sensor_elapsed_us += elapsed_us(
                    collected[-1].timestamp_us, packet.timestamp_us
                )
            collected.append(packet)
            if sensor_elapsed_us >= duration_us:
                return collected
    raise TimeoutError(
        "the sensor stream did not reach the requested duration; "
        f"received_packets={len(collected)}"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--port", help="serial port, for example COM3; auto-detected if omitted"
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=DEFAULT_DURATION_SECONDS,
        help="sensor-time verification window in seconds (default: 5)",
    )
    parser.add_argument("--json", type=Path, help="also write the report as JSON")
    args = parser.parse_args()
    if args.duration <= 0 or args.duration >= 600:
        parser.error("--duration must be greater than 0 and less than 600 seconds")

    decoder = StreamDecoder()
    packets: list[ImuPacket] = []
    try:
        port_name = args.port or detect_port()
        print(f"port={port_name}")
        with serial.Serial(
            port_name, SERIAL_BAUD, timeout=READ_TIMEOUT_SECONDS
        ) as port:
            port.dtr = True
            port.rts = True
            time.sleep(0.2)
            port.reset_input_buffer()
            source = SerialSource(port)
            if not wait_for_banner(source, decoder, BANNER_TIMEOUT_SECONDS):
                raise TimeoutError(
                    "no firmware banner received; confirm that "
                    "kPresentationStreamEnabled is true in src/config/constants.h"
                )
            if decoder.device.streaming:
                print(f"keep_the_device_still duration_s={args.duration:g}")
                packets = collect_window(
                    source,
                    decoder,
                    args.duration,
                    args.duration + COLLECT_TIMEOUT_MARGIN_SECONDS,
                )
    except (OSError, RuntimeError, TimeoutError, serial.SerialException) as exc:
        print(f"verification_failed: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc

    verdicts, overall = evaluate_all(
        packets, decoder.stats, decoder.device, decoder.banner_bytes
    )
    print(format_report(decoder.device, verdicts, overall))
    if args.json is not None:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        document = report_document(decoder.device, verdicts, overall)
        args.json.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    raise SystemExit(1 if overall == FAIL else 0)


if __name__ == "__main__":
    main()
