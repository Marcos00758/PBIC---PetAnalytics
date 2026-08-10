"""Validate and plot a PBIC capture containing three ICM-20948 sensors."""

from __future__ import annotations

import argparse
import json
import math
import statistics
from dataclasses import dataclass
from pathlib import Path

from parse_data import (
    BMP_RAW_INVALID,
    DEFAULT_GYRO_RANGE_DPS,
    PACKET_SIZE,
    MagnetometerCalibration,
    ParseStats,
    ParsedPacket,
    elapsed_us,
    iter_file_packets,
)

MAX_PLOT_POINTS = 50_000
RAW_SATURATION_THRESHOLD = 32_000
GYRO_RANGE_BY_PACKET_VERSION = {3: 1000.0, 4: 2000.0}


@dataclass(frozen=True)
class TimingStats:
    duration_s: float
    effective_rate_hz: float
    mean_period_ms: float
    jitter_std_ms: float
    min_period_ms: float
    max_period_ms: float


@dataclass(frozen=True)
class SensorDiagnostics:
    accel_near_limit: tuple[int, ...]
    gyro_near_limit: tuple[int, ...]
    mag_changes: tuple[int, ...]
    mag_min_interval_packets: tuple[int, ...]
    mag_max_interval_packets: tuple[int, ...]
    mag_norm_min_ut: tuple[float, ...]
    mag_norm_max_ut: tuple[float, ...]
    bmp_pressure_ranges: tuple[tuple[int, int], ...]
    bmp_temperature_ranges: tuple[tuple[int, int], ...]
    bmp_changes: tuple[int, ...]
    bmp_invalid_packets: tuple[int, ...]


def calculate_timing(packets: list[ParsedPacket]) -> TimingStats | None:
    if len(packets) < 2:
        return None

    intervals_us = [
        elapsed_us(previous.packet.timestamp_us, current.packet.timestamp_us)
        for previous, current in zip(packets, packets[1:])
    ]
    duration_us = sum(intervals_us)
    mean_us = statistics.fmean(intervals_us)
    jitter_us = statistics.pstdev(intervals_us)
    rate_hz = (len(intervals_us) * 1_000_000 / duration_us) if duration_us else math.inf
    return TimingStats(
        duration_s=duration_us / 1_000_000,
        effective_rate_hz=rate_hz,
        mean_period_ms=mean_us / 1_000,
        jitter_std_ms=jitter_us / 1_000,
        min_period_ms=min(intervals_us) / 1_000,
        max_period_ms=max(intervals_us) / 1_000,
    )


def analyze_file(
    path: Path, max_plot_points: int = MAX_PLOT_POINTS
) -> tuple[list[ParsedPacket], ParseStats, TimingStats | None, SensorDiagnostics]:
    if max_plot_points <= 0:
        raise ValueError("max_plot_points must be positive")

    estimated_packets = max(1, path.stat().st_size // PACKET_SIZE)
    plot_stride = max(1, math.ceil(estimated_packets / max_plot_points))
    stats = ParseStats()
    sampled: list[ParsedPacket] = []
    last_parsed: ParsedPacket | None = None

    interval_count = 0
    interval_mean = 0.0
    interval_m2 = 0.0
    interval_total = 0
    interval_min: int | None = None
    interval_max: int | None = None
    previous_timestamp: int | None = None

    accel_near_limit = [0, 0, 0]
    gyro_near_limit = [0, 0, 0]
    mag_changes = [0, 0, 0]
    mag_previous: list[tuple[int, int, int] | None] = [None, None, None]
    mag_last_change: list[int | None] = [None, None, None]
    mag_min_interval = [0, 0, 0]
    mag_max_interval = [0, 0, 0]
    mag_norm_min = [math.inf, math.inf, math.inf]
    mag_norm_max = [0.0, 0.0, 0.0]
    bmp_pressure_min = [0xFFFFFFFF, 0xFFFFFFFF]
    bmp_pressure_max = [0, 0]
    bmp_temperature_min = [0xFFFFFFFF, 0xFFFFFFFF]
    bmp_temperature_max = [0, 0]
    bmp_previous: list[tuple[int, int] | None] = [None, None]
    bmp_changes = [0, 0]
    bmp_invalid_packets = [0, 0]

    for packet_index, parsed in enumerate(iter_file_packets(path, stats)):
        packet = parsed.packet
        last_parsed = parsed
        if packet_index % plot_stride == 0:
            sampled.append(parsed)

        if previous_timestamp is not None:
            interval = elapsed_us(previous_timestamp, packet.timestamp_us)
            interval_count += 1
            interval_total += interval
            delta = interval - interval_mean
            interval_mean += delta / interval_count
            interval_m2 += delta * (interval - interval_mean)
            interval_min = interval if interval_min is None else min(interval_min, interval)
            interval_max = interval if interval_max is None else max(interval_max, interval)
        previous_timestamp = packet.timestamp_us

        for sensor in range(3):
            base = sensor * 9
            accel_near_limit[sensor] += sum(
                abs(value) >= RAW_SATURATION_THRESHOLD
                for value in packet.values[base : base + 3]
            )
            gyro_near_limit[sensor] += sum(
                abs(value) >= RAW_SATURATION_THRESHOLD
                for value in packet.values[base + 3 : base + 6]
            )
            magnetic = packet.values[base + 6 : base + 9]
            norm_ut = 0.15 * math.sqrt(sum(value * value for value in magnetic))
            mag_norm_min[sensor] = min(mag_norm_min[sensor], norm_ut)
            mag_norm_max[sensor] = max(mag_norm_max[sensor], norm_ut)
            if mag_previous[sensor] is not None and magnetic != mag_previous[sensor]:
                mag_changes[sensor] += 1
                if mag_last_change[sensor] is not None:
                    change_interval = packet_index - mag_last_change[sensor]
                    current_min = mag_min_interval[sensor]
                    mag_min_interval[sensor] = (
                        change_interval if current_min == 0 else min(current_min, change_interval)
                    )
                    mag_max_interval[sensor] = max(
                        mag_max_interval[sensor], change_interval
                    )
                mag_last_change[sensor] = packet_index
            mag_previous[sensor] = magnetic

        for sensor, (pressure, temperature) in enumerate(packet.bmp_raw):
            if pressure == BMP_RAW_INVALID or temperature == BMP_RAW_INVALID:
                bmp_invalid_packets[sensor] += 1
                continue
            bmp_pressure_min[sensor] = min(bmp_pressure_min[sensor], pressure)
            bmp_pressure_max[sensor] = max(bmp_pressure_max[sensor], pressure)
            bmp_temperature_min[sensor] = min(bmp_temperature_min[sensor], temperature)
            bmp_temperature_max[sensor] = max(bmp_temperature_max[sensor], temperature)
            current_bmp = (pressure, temperature)
            if bmp_previous[sensor] is not None and current_bmp != bmp_previous[sensor]:
                bmp_changes[sensor] += 1
            bmp_previous[sensor] = current_bmp

    if last_parsed is not None and (not sampled or sampled[-1].offset != last_parsed.offset):
        sampled.append(last_parsed)

    timing = None
    if interval_count > 0 and interval_min is not None and interval_max is not None:
        jitter = math.sqrt(interval_m2 / interval_count)
        timing = TimingStats(
            duration_s=interval_total / 1_000_000,
            effective_rate_hz=interval_count * 1_000_000 / interval_total,
            mean_period_ms=interval_mean / 1_000,
            jitter_std_ms=jitter / 1_000,
            min_period_ms=interval_min / 1_000,
            max_period_ms=interval_max / 1_000,
        )

    diagnostics = SensorDiagnostics(
        accel_near_limit=tuple(accel_near_limit),
        gyro_near_limit=tuple(gyro_near_limit),
        mag_changes=tuple(mag_changes),
        mag_min_interval_packets=tuple(mag_min_interval),
        mag_max_interval_packets=tuple(mag_max_interval),
        mag_norm_min_ut=tuple(0.0 if value == math.inf else value for value in mag_norm_min),
        mag_norm_max_ut=tuple(mag_norm_max),
        bmp_pressure_ranges=tuple(zip(bmp_pressure_min, bmp_pressure_max)),
        bmp_temperature_ranges=tuple(zip(bmp_temperature_min, bmp_temperature_max)),
        bmp_changes=tuple(bmp_changes),
        bmp_invalid_packets=tuple(bmp_invalid_packets),
    )
    return sampled, stats, timing, diagnostics


def format_validation(stats: ParseStats, timing: TimingStats | None) -> str:
    lines = [
        f"packet_size={PACKET_SIZE}",
        f"stream_bytes={stats.valid_packets * PACKET_SIZE}",
        f"valid_packets={stats.valid_packets}",
        f"crc_failures={stats.crc_failures}",
        f"sequence_gaps={stats.sequence_gaps}",
        f"discarded_bytes={stats.discarded_bytes}",
        f"trailing_bytes={stats.trailing_bytes}",
    ]
    if timing is not None:
        lines.extend(
            (
                f"duration_s={timing.duration_s:.6f}",
                f"effective_rate_hz={timing.effective_rate_hz:.3f}",
                f"mean_period_ms={timing.mean_period_ms:.3f}",
                f"jitter_std_ms={timing.jitter_std_ms:.3f}",
                f"min_period_ms={timing.min_period_ms:.3f}",
                f"max_period_ms={timing.max_period_ms:.3f}",
                f"effective_usb_bytes_per_s={timing.effective_rate_hz * PACKET_SIZE:.1f}",
            )
        )
    return "\n".join(lines)


def format_sensor_diagnostics(
    diagnostics: SensorDiagnostics, timing: TimingStats | None
) -> str:
    duration = timing.duration_s if timing is not None else 0.0
    lines = []
    for sensor in range(3):
        update_rate = diagnostics.mag_changes[sensor] / duration if duration else 0.0
        lines.append(
            f"icm{sensor}_near_limit accel={diagnostics.accel_near_limit[sensor]} "
            f"gyro={diagnostics.gyro_near_limit[sensor]} "
            f"mag_changes={diagnostics.mag_changes[sensor]} "
            f"mag_change_rate_hz={update_rate:.3f} "
            f"mag_interval_packets={diagnostics.mag_min_interval_packets[sensor]}.."
            f"{diagnostics.mag_max_interval_packets[sensor]} "
            f"mag_norm_ut={diagnostics.mag_norm_min_ut[sensor]:.2f}.."
            f"{diagnostics.mag_norm_max_ut[sensor]:.2f}"
        )
    for sensor in range(2):
        update_rate = diagnostics.bmp_changes[sensor] / duration if duration else 0.0
        lines.append(
            f"bmp{sensor}_raw pressure={diagnostics.bmp_pressure_ranges[sensor][0]}.."
            f"{diagnostics.bmp_pressure_ranges[sensor][1]} temperature="
            f"{diagnostics.bmp_temperature_ranges[sensor][0]}.."
            f"{diagnostics.bmp_temperature_ranges[sensor][1]} "
            f"changes={diagnostics.bmp_changes[sensor]} "
            f"change_rate_hz={update_rate:.3f} "
            f"invalid_packets={diagnostics.bmp_invalid_packets[sensor]}"
        )
    return "\n".join(lines)


def load_magnetometer_calibrations(
    path: Path | None,
) -> tuple[MagnetometerCalibration, ...]:
    if path is None:
        return (MagnetometerCalibration(),) * 3

    document = json.loads(path.read_text(encoding="utf-8"))
    calibrations = []
    for sensor in range(3):
        entry = document.get(f"icm{sensor}", {})
        hard_iron = tuple(float(value) for value in entry.get("hard_iron_ut", (0, 0, 0)))
        matrix = tuple(
            tuple(float(value) for value in row)
            for row in entry.get(
                "soft_iron_matrix", ((1, 0, 0), (0, 1, 0), (0, 0, 1))
            )
        )
        if len(hard_iron) != 3 or len(matrix) != 3 or any(len(row) != 3 for row in matrix):
            raise ValueError(f"invalid magnetometer calibration for icm{sensor}")
        calibrations.append(MagnetometerCalibration(hard_iron, matrix))
    return tuple(calibrations)


def load_gyro_range_dps(path: Path) -> tuple[int | None, float]:
    metadata_path = path.with_suffix(path.suffix + ".json")
    if not metadata_path.exists():
        return None, DEFAULT_GYRO_RANGE_DPS
    document = json.loads(metadata_path.read_text(encoding="utf-8"))
    version = int(document["packet_version"])
    if version not in GYRO_RANGE_BY_PACKET_VERSION:
        raise ValueError(f"unsupported packet version in metadata: {version}")
    return version, GYRO_RANGE_BY_PACKET_VERSION[version]


def plot_capture(
    packets: list[ParsedPacket],
    stats: ParseStats,
    timing: TimingStats,
    output: Path | None,
    show: bool,
    mag_output: Path | None = None,
    mag_calibrations: tuple[MagnetometerCalibration, ...] | None = None,
    bmp_output: Path | None = None,
    gyro_range_dps: float = DEFAULT_GYRO_RANGE_DPS,
) -> None:
    try:
        import matplotlib.pyplot as plt
    except ImportError as exc:
        raise RuntimeError(
            "matplotlib is required; install it with: "
            "pip install -r python/requirements.txt"
        ) from exc

    first_timestamp = packets[0].packet.timestamp_us
    times_s = [elapsed_us(first_timestamp, item.packet.timestamp_us) / 1_000_000 for item in packets]
    physical = [
        item.packet.physical_values(mag_calibrations, gyro_range_dps)
        for item in packets
    ]

    figure, axes = plt.subplots(2, 3, figsize=(18, 8), sharex=True)
    for sensor in range(3):
        accel_axis = axes[0][sensor]
        gyro_axis = axes[1][sensor]
        for axis in ("x", "y", "z"):
            accel_axis.plot(times_s, [row[f"icm{sensor}_a{axis}"] for row in physical], label=f"a{axis}", linewidth=1)
            gyro_axis.plot(times_s, [row[f"icm{sensor}_g{axis}"] for row in physical], label=f"g{axis}", linewidth=1)

        accel_axis.set_title(f"ICM#{sensor} - Acelerometro")
        gyro_axis.set_title(f"ICM#{sensor} - Giroscopio")
        accel_axis.set_ylabel("Aceleracao (m/s2)")
        gyro_axis.set_ylabel("Giro (graus/s)")
        gyro_axis.set_xlabel("Tempo (s)")
        accel_axis.grid(True, alpha=0.45)
        gyro_axis.grid(True, alpha=0.45)
        accel_axis.legend()
        gyro_axis.legend()

    figure.suptitle(
        f"IMU Triple - {stats.valid_packets} pacotes | fs={timing.effective_rate_hz:.2f} Hz | "
        f"periodo={timing.mean_period_ms:.3f} ms | jitter sigma={timing.jitter_std_ms:.3f} ms\n"
        f"gaps={stats.sequence_gaps} | CRC invalidos={stats.crc_failures} | "
        "colunas: ICM#0 (mux ch0), ICM#1 (mux ch1), ICM#2 (mux ch4)"
    )
    figure.tight_layout(rect=(0, 0, 1, 0.92))

    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
        figure.savefig(output, dpi=160, bbox_inches="tight")

    mag_figure, mag_axes = plt.subplots(1, 3, figsize=(18, 5), sharex=True)
    for sensor, mag_axis in enumerate(mag_axes):
        for axis in ("x", "y", "z"):
            mag_axis.plot(
                times_s,
                [row[f"icm{sensor}_m{axis}"] for row in physical],
                label=f"m{axis}",
                linewidth=1,
            )
        mag_axis.set_title(f"ICM#{sensor} - Magnetometro")
        mag_axis.set_ylabel("Campo magnetico (uT)")
        mag_axis.set_xlabel("Tempo (s)")
        mag_axis.grid(True, alpha=0.45)
        mag_axis.legend()
    mag_figure.suptitle(
        f"AK09916 Triple - cache 20 Hz em pacotes de 100 Hz | "
        f"{stats.valid_packets} pacotes"
    )
    mag_figure.tight_layout(rect=(0, 0, 1, 0.93))
    if mag_output is not None:
        mag_output.parent.mkdir(parents=True, exist_ok=True)
        mag_figure.savefig(mag_output, dpi=160, bbox_inches="tight")

    bmp_figure, bmp_axes = plt.subplots(2, 1, figsize=(14, 8), sharex=True)
    for sensor in range(2):
        bmp_axes[0].plot(
            times_s,
            [
                math.nan
                if item.packet.bmp_raw[sensor][0] == BMP_RAW_INVALID
                else item.packet.bmp_raw[sensor][0]
                for item in packets
            ],
            label=f"BMP#{sensor}",
            linewidth=1,
        )
        bmp_axes[1].plot(
            times_s,
            [
                math.nan
                if item.packet.bmp_raw[sensor][1] == BMP_RAW_INVALID
                else item.packet.bmp_raw[sensor][1]
                for item in packets
            ],
            label=f"BMP#{sensor}",
            linewidth=1,
        )
    bmp_axes[0].set_title("BMP390 - Pressao crua em cache")
    bmp_axes[1].set_title("BMP390 - Temperatura crua em cache")
    bmp_axes[0].set_ylabel("Contagem raw 24-bit")
    bmp_axes[1].set_ylabel("Contagem raw 24-bit")
    bmp_axes[1].set_xlabel("Tempo (s)")
    for axis in bmp_axes:
        axis.grid(True, alpha=0.45)
        axis.legend()
    bmp_figure.tight_layout()
    if bmp_output is not None:
        bmp_output.parent.mkdir(parents=True, exist_ok=True)
        bmp_figure.savefig(bmp_output, dpi=160, bbox_inches="tight")
    if show:
        plt.show()
    else:
        plt.close(figure)
        plt.close(mag_figure)
        plt.close(bmp_figure)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="raw .bin capture")
    parser.add_argument("--output", type=Path, help="PNG output path")
    parser.add_argument(
        "--mag-output", type=Path, help="magnetometer PNG output path"
    )
    parser.add_argument("--bmp-output", type=Path, help="raw BMP390 PNG output path")
    parser.add_argument(
        "--mag-calibration",
        type=Path,
        help="JSON containing hard-iron offsets and soft-iron matrices",
    )
    parser.add_argument("--no-show", action="store_true", help="do not open the plot window")
    args = parser.parse_args()

    packets, stats, timing, diagnostics = analyze_file(args.input)
    print(format_validation(stats, timing))
    print(format_sensor_diagnostics(diagnostics, timing))
    if timing is None:
        raise SystemExit("at least two valid packets are required to plot a capture")

    output = args.output or args.input.with_suffix(".png")
    mag_output = args.mag_output or args.input.with_name(
        f"{args.input.stem}_mag.png"
    )
    bmp_output = args.bmp_output or args.input.with_name(
        f"{args.input.stem}_bmp_raw.png"
    )
    try:
        calibrations = load_magnetometer_calibrations(args.mag_calibration)
        packet_version, gyro_range_dps = load_gyro_range_dps(args.input)
        print(
            f"packet_version={packet_version if packet_version is not None else 'unknown'} "
            f"gyro_range_dps={gyro_range_dps:g}"
        )
        plot_capture(
            packets,
            stats,
            timing,
            output,
            not args.no_show,
            mag_output,
            calibrations,
            bmp_output,
            gyro_range_dps,
        )
    except (OSError, ValueError, json.JSONDecodeError, RuntimeError) as exc:
        raise SystemExit(str(exc)) from exc
    print(f"plot_file={output.resolve()}")
    print(f"mag_plot_file={mag_output.resolve()}")
    print(f"bmp_plot_file={bmp_output.resolve()}")


if __name__ == "__main__":
    main()
