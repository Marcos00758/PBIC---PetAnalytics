"""Report raw ICM statistics from the confirmed prefix of an SD session."""

import argparse
from dataclasses import dataclass, field
import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from analyze_imu import RAW_SATURATION_THRESHOLD
from parse_data import (
    MAG_UT_PER_COUNT, PACKET_SIZE, ParseStats, elapsed_us, iter_binary_packets,
    load_session_journal, load_session_metadata, load_session_status, summarize_timestamps,
)

GROUPS = ("accel", "gyro", "mag")
UNITS = ("g", "dps", "ut")
PAIRS = ((0, 1), (0, 2), (1, 2))


@dataclass
class Metric:
    count: int = 0
    mean: float = 0.0
    m2: float = 0.0
    minimum: float = math.inf
    maximum: float = -math.inf

    def add(self, value):
        self.count += 1
        delta = value - self.mean
        self.mean += delta / self.count
        self.m2 += delta * (value - self.mean)
        self.minimum = min(self.minimum, value)
        self.maximum = max(self.maximum, value)

    def describe(self, scale=1.0, *, precision=6):
        if not self.count or scale is None:
            return "unavailable"
        std = math.sqrt(max(0.0, self.m2 / self.count))
        return (f"min={self.minimum * scale:.{precision}g} max={self.maximum * scale:.{precision}g} "
                f"mean={self.mean * scale:.{precision}g} std={std * scale:.{precision}g}")


@dataclass
class SensorMetrics:
    axes: list = field(default_factory=lambda: [Metric() for _ in range(9)])
    norms: list = field(default_factory=lambda: [Metric() for _ in GROUPS])
    near_limit: list = field(default_factory=lambda: [0, 0])
    changes: list = field(default_factory=lambda: [0, 0, 0])
    previous: list = field(default_factory=lambda: [None, None, None])
    last_change_s: list = field(default_factory=lambda: [0.0, 0.0, 0.0])
    longest_unchanged_s: list = field(default_factory=lambda: [0.0, 0.0, 0.0])
    mag_zero_packets: int = 0

    def add(self, values, time_s):
        for metric, value in zip(self.axes, values):
            metric.add(value)
        norms = []
        for group in range(3):
            vector = values[group * 3:group * 3 + 3]
            norm = math.sqrt(sum(value * value for value in vector))
            self.norms[group].add(norm)
            norms.append(norm)
            if group < 2:
                self.near_limit[group] += sum(abs(v) >= RAW_SATURATION_THRESHOLD for v in vector)
            if vector != self.previous[group]:
                self.changes[group] += int(self.previous[group] is not None)
                self.last_change_s[group] = time_s
            else:
                self.longest_unchanged_s[group] = max(
                    self.longest_unchanged_s[group], time_s - self.last_change_s[group])
            self.previous[group] = vector
        self.mag_zero_packets += int(not any(values[6:9]))
        return norms


def load_capture(path):
    path = path / "imu.bin" if path.is_dir() else path
    metadata = load_session_metadata(path)
    if metadata.get("packet_version") != "4" or metadata.get("packet_size") != str(PACKET_SIZE):
        raise ValueError("meta.txt deve declarar packet_version=4 e packet_size=79")
    journal = load_session_journal(path, binary_only=True)
    try:
        prefix = int(journal["imu_valid_bytes"])
    except (KeyError, ValueError) as exc:
        raise ValueError("journal.bin sem checkpoint binario valido") from exc
    if not 0 <= prefix <= path.stat().st_size:
        raise ValueError("prefixo do journal fora dos limites de imu.bin")
    return path, metadata, prefix


def load_scales(metadata):
    ranges = []
    for name, key, allowed in (
        ("accel", "accel_range_g", (2, 4, 8, 16)),
        ("gyro", "gyro_range_dps", (250, 500, 1000, 2000)),
    ):
        try:
            value = float(metadata[key])
        except (KeyError, ValueError):
            value = None
        ranges.append(value / 32768.0 if value in allowed else None)
        if value in allowed:
            expected = allowed.index(value) << 1
            for sensor in range(3):
                encoded = metadata.get(f"icm{sensor}_{name}_config")
                if encoded is not None and int(encoded, 0) & 0x06 != expected:
                    raise ValueError(f"faixa {key} diverge do readback do ICM{sensor}")
    return (*ranges, MAG_UT_PER_COUNT)


def sensor_report(sensors, scales, metadata, rest):
    lines = []
    for index, sensor in enumerate(sensors):
        lines.append(f"\nICM{index} channel={metadata.get(f'icm{index}_channel', 'unknown')} "
                     f"ready={metadata.get(f'icm{index}_ready', 'unknown')} "
                     f"config_verified={metadata.get(f'icm{index}_config_verified', 'unknown')}")
        lines.append("  registers " + " ".join(
            f"{name}={metadata.get(f'icm{index}_{name}', 'unknown')}"
            for name in ("accel_config", "gyro_config", "mag_mode")))
        for group, (name, unit, scale) in enumerate(zip(GROUPS, UNITS, scales)):
            for axis, metric in zip("xyz", sensor.axes[group * 3:group * 3 + 3]):
                lines.append(f"  {name}_{axis} raw_counts {metric.describe()} "
                             f"| {unit} {metric.describe(scale)}")
            lines.append(f"  {name}_norm_{unit} {sensor.norms[group].describe(scale)} "
                         f"changes={sensor.changes[group]} "
                         f"longest_unchanged_observed_s={sensor.longest_unchanged_s[group]:.6f}")
        lines.append(f"  near_limit_axis_samples accel={sensor.near_limit[0]} "
                     f"gyro={sensor.near_limit[1]} raw_threshold={RAW_SATURATION_THRESHOLD}")
        lines.append(f"  mag_zero_packets={sensor.mag_zero_packets}")
    lines.append("\nrest_declared=" + str(int(rest)))
    lines.append("gyro_mean_interpretation=" + ("rest_offset" if rest else "mean_only"))
    return lines


def snapshot_report(path):
    status = load_session_status(path)
    lines = [f"\nSTATUS_SNAPSHOT uptime_ms={status.get('uptime_ms', 'unknown')} "
             f"packets_produced={status.get('packets_produced', 'unknown')}"]
    lines.extend(f"  {key}={value}" for key, value in status.items()
                 if key.startswith(("icm_i2c_failures_", "mag_")))
    lines.append("snapshot_is_periodic_not_per_packet_or_selected_window")
    return lines


def analyze_capture(path, start_s=0.0, end_s=None, rest=False):
    if not math.isfinite(start_s) or start_s < 0 or (
        end_s is not None and (not math.isfinite(end_s) or end_s <= start_s)
    ):
        raise ValueError("janela deve ter inicio >= 0 e fim > inicio, ambos finitos")
    path, metadata, prefix = load_capture(Path(path))
    scales = load_scales(metadata)
    stats, intervals = ParseStats(), Metric()
    sensors = [SensorMetrics() for _ in range(3)]
    differences = [[Metric() for _ in GROUPS] for _ in PAIRS]
    previous = None
    duration_us = selected = bad_sequence = bad_time = 0
    first_selected_s = last_selected_s = None
    with path.open("rb") as stream:
        for parsed in iter_binary_packets(stream, stats, max_bytes=prefix):
            packet = parsed.packet
            if previous is not None:
                interval = elapsed_us(previous.timestamp_us, packet.timestamp_us)
                intervals.add(interval)
                bad_time += int(interval == 0 or interval >= 0x80000000)
                delta = (packet.sequence - previous.sequence) & 0xFFFF
                bad_sequence += int(delta == 0 or delta >= 0x8000)
                duration_us += interval
            previous = packet
            time_s = duration_us / 1_000_000
            if time_s < start_s or (end_s is not None and time_s >= end_s):
                continue
            selected += 1
            if first_selected_s is None:
                first_selected_s = time_s
            last_selected_s = time_s
            norms = [sensor.add(packet.values[i * 9:i * 9 + 9], time_s)
                     for i, sensor in enumerate(sensors)]
            for pair, (left, right) in zip(differences, PAIRS):
                for group, metric in enumerate(pair):
                    metric.add(abs(norms[left][group] - norms[right][group]))

    damaged = any((stats.crc_failures, stats.discarded_bytes, stats.sequence_gaps,
                   bad_sequence, bad_time))
    status = "failed" if damaged else "insufficient" if selected < 2 else "passed"
    span = last_selected_s - first_selected_s if selected else 0.0
    lines = ["evaluation=icm report_version=1", f"input={path.resolve()}",
             f"confirmed_prefix_bytes={prefix}",
             f"ignored_unconfirmed_bytes={path.stat().st_size - prefix}",
             summarize_timestamps(stats, duration_us if stats.valid_packets >= 2 else None),
             f"interval_ms {intervals.describe(0.001)}",
             f"sequence_duplicate_or_ambiguous={bad_sequence}", f"timestamp_inconsistencies={bad_time}",
             f"capture_integrity={status} scope=full_confirmed_prefix",
             f"selected_packets={selected} selected_span_s={span:.6f}",
             f"window_start_s={start_s} window_end_s={end_s if end_s is not None else 'end'}",
             f"physical_scales={'available' if None not in scales else 'incomplete_raw_only_for_missing_ranges'}",
             f"dlpf_enabled={metadata.get('icm_dlpf_enabled', 'unknown')} "
             f"accel_internal_nominal_hz={metadata.get('icm_accel_internal_rate_hz', 'unknown')} "
             f"gyro_internal_nominal_hz={metadata.get('icm_gyro_internal_rate_hz', 'unknown')}"]
    lines.extend(sensor_report(sensors, scales, metadata, rest))
    lines.append("\nCOMPARISON vector_norms_only axes_alignment_not_assumed")
    for pair, (left, right) in zip(differences, PAIRS):
        for name, unit, scale, metric in zip(GROUPS, UNITS, scales, pair):
            lines.append(f"  icm{left}_icm{right} {name}_norm_abs_difference_{unit} {metric.describe(scale)}")
    lines.extend(snapshot_report(path))
    lines.extend(("\nphysical_suitability=inconclusive_reference_required",
                  "mag_changes_are_not_odr; repetitions_are_not_proof_of_stale_data",
                  "timestamps_are_per_round; filters_and_calibration_not_applied"))
    return "\n".join(lines), 0 if status == "passed" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="pasta da sessao SD ou imu.bin")
    parser.add_argument("--start-s", type=float, default=0.0, help="inicio desde o primeiro pacote valido")
    parser.add_argument("--end-s", type=float, help="fim exclusivo da janela, em segundos")
    parser.add_argument("--rest", action="store_true", help="declarar a janela selecionada como repouso")
    args = parser.parse_args()
    try:
        report, code = analyze_capture(args.input, args.start_s, args.end_s, args.rest)
    except (OSError, ValueError) as exc:
        print(f"avaliacao_icm: {exc}", file=sys.stderr)
        return 2
    print(report)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
