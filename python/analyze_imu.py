"""Validate and plot a PBIC capture containing three ICM-20948 sensors."""

from __future__ import annotations

import argparse
import json
import math
import statistics
from dataclasses import dataclass
from pathlib import Path

from parse_data import (
    PACKET_SIZE,
    MagnetometerCalibration,
    ParseStats,
    ParsedPacket,
    elapsed_us,
    parse_stream,
)


@dataclass(frozen=True)
class TimingStats:
    duration_s: float
    effective_rate_hz: float
    mean_period_ms: float
    jitter_std_ms: float
    min_period_ms: float
    max_period_ms: float


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


def plot_capture(
    packets: list[ParsedPacket],
    stats: ParseStats,
    timing: TimingStats,
    output: Path | None,
    show: bool,
    mag_output: Path | None = None,
    mag_calibrations: tuple[MagnetometerCalibration, ...] | None = None,
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
        item.packet.physical_values(mag_calibrations) for item in packets
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
    if show:
        plt.show()
    else:
        plt.close(figure)
        plt.close(mag_figure)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="raw .bin capture")
    parser.add_argument("--output", type=Path, help="PNG output path")
    parser.add_argument(
        "--mag-output", type=Path, help="magnetometer PNG output path"
    )
    parser.add_argument(
        "--mag-calibration",
        type=Path,
        help="JSON containing hard-iron offsets and soft-iron matrices",
    )
    parser.add_argument("--no-show", action="store_true", help="do not open the plot window")
    args = parser.parse_args()

    packets, stats = parse_stream(args.input.read_bytes())
    timing = calculate_timing(packets)
    print(format_validation(stats, timing))
    if timing is None:
        raise SystemExit("at least two valid packets are required to plot a capture")

    output = args.output or args.input.with_suffix(".png")
    mag_output = args.mag_output or args.input.with_name(
        f"{args.input.stem}_mag.png"
    )
    try:
        calibrations = load_magnetometer_calibrations(args.mag_calibration)
        plot_capture(
            packets,
            stats,
            timing,
            output,
            not args.no_show,
            mag_output,
            calibrations,
        )
    except (OSError, ValueError, json.JSONDecodeError, RuntimeError) as exc:
        raise SystemExit(str(exc)) from exc
    print(f"plot_file={output.resolve()}")
    print(f"mag_plot_file={mag_output.resolve()}")


if __name__ == "__main__":
    main()
