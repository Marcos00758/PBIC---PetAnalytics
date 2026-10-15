"""Evaluate raw BMP390 data from the journal-confirmed prefix of an SD session."""

import argparse
from dataclasses import dataclass, field
import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from avaliacao.avaliar_icm import Metric, load_capture
from parse_data import (
    BMP_RAW_INVALID, ParseStats, elapsed_us, iter_binary_packets,
    load_bmp390_calibrations, load_session_status, summarize_timestamps,
)


@dataclass
class Trend:
    values: Metric = field(default_factory=Metric)
    times: Metric = field(default_factory=Metric)
    covariance: float = 0.0

    def add(self, time_s, value):
        delta_time = time_s - self.times.mean
        self.times.add(time_s)
        self.values.add(value)
        self.covariance += delta_time * (value - self.values.mean)

    def describe(self):
        slope = (f"{60 * self.covariance / self.times.m2:.6g}"
                 if self.times.m2 > 0 else "unavailable")
        return f"{self.values.describe(precision=9)} slope_per_min={slope}"


@dataclass
class Timing:
    previous: object = None
    duration_us: int = 0
    sequence_errors: int = 0
    timestamp_errors: int = 0
    intervals: Metric = field(default_factory=Metric)

    def add(self, packet):
        if self.previous is not None:
            delta = elapsed_us(self.previous.timestamp_us, packet.timestamp_us)
            self.duration_us += delta
            self.intervals.add(delta)
            self.timestamp_errors += int(delta == 0 or delta >= 0x80000000)
            sequence_delta = (packet.sequence - self.previous.sequence) & 0xFFFF
            self.sequence_errors += int(sequence_delta == 0 or sequence_delta >= 0x8000)
        self.previous = packet
        return self.duration_us / 1_000_000


def timed_packets(path, prefix, stats, timing):
    with path.open("rb") as stream:
        for parsed in iter_binary_packets(stream, stats, max_bytes=prefix):
            yield timing.add(parsed.packet), parsed.packet


def in_window(time_s, window):
    return time_s >= window[0] and (window[1] is None or time_s < window[1])


def validate_window(window):
    start, end = window
    if not math.isfinite(start) or start < 0 or (
        end is not None and (not math.isfinite(end) or end <= start)
    ):
        raise ValueError("janela deve ter inicio >= 0 e fim > inicio, ambos finitos")


def calibrations_with_status(path, metadata):
    calibrations, reasons = list(load_bmp390_calibrations(path)), []
    for index in range(2):
        encoded = metadata.get(f"bmp{index}_nvm", "")
        reason = "available"
        if metadata.get(f"bmp{index}_nvm_valid") != "1":
            reason = "nvm_not_declared_valid"
        elif calibrations[index] is None:
            reason = "nvm_missing_or_malformed"
        elif bytes.fromhex(encoded) in (bytes(21), bytes([255]) * 21):
            reason = "nvm_empty_pattern"
        elif metadata.get(f"bmp{index}_ready") == "0":
            reason = "sensor_not_ready"
        elif metadata.get(f"bmp{index}_address", "0x77") not in ("0x76", "0x77"):
            reason = "unsupported_declared_address"
        if reason != "available":
            calibrations[index] = None
        reasons.append(reason)
    return calibrations, reasons


def raw_error(raw):
    if BMP_RAW_INVALID in raw:
        return "sentinel"
    return "out_of_24bit" if any(not 0 <= value <= 0xFFFFFF for value in raw) else None


def compensate(raw, calibration):
    if calibration is None or raw_error(raw):
        return None
    pressure, temperature = calibration.compensate(*raw)
    if not (math.isfinite(pressure) and math.isfinite(temperature)
            and 30_000 <= pressure <= 125_000 and -40 <= temperature <= 85):
        return None
    return pressure, temperature


@dataclass
class BmpMetrics:
    raw: list = field(default_factory=lambda: [Metric(), Metric()])
    physical: list = field(default_factory=lambda: [Trend(), Trend()])
    height: Trend = field(default_factory=Trend)
    sentinels: int = 0
    out_of_24bit: int = 0
    invalid_physical: int = 0
    changes: int = 0
    longest_unchanged_s: float = 0.0
    previous: object = None
    last_change_s: float = 0.0

    def add(self, raw, physical, time_s, calibration):
        error = raw_error(raw)
        if error:
            self.sentinels += int(error == "sentinel")
            self.out_of_24bit += int(error == "out_of_24bit")
            self.previous = None
            return
        for metric, value in zip(self.raw, raw):
            metric.add(value)
        if raw != self.previous:
            self.changes += int(self.previous is not None)
            self.last_change_s = time_s
        else:
            self.longest_unchanged_s = max(self.longest_unchanged_s,
                                          time_s - self.last_change_s)
        self.previous = raw
        if physical is not None:
            for metric, value in zip(self.physical, physical):
                metric.add(time_s, value)
        elif calibration is not None:
            self.invalid_physical += 1


def reference_constants(references, unusable, window, timing, air_temperature_c):
    constants = [None, None]
    if window is None:
        return constants
    # A requested reference must be fully inside the observed capture.
    captured_end = (timing.duration_us + timing.intervals.mean) / 1_000_000
    if window[1] > captured_end:
        return constants
    for index, (pressure, temperature) in enumerate(references):
        if pressure.count >= 2 and not unusable[index]:
            air_c = temperature.mean if air_temperature_c is None else air_temperature_c
            constants[index] = (pressure.mean, 287.05 * (air_c + 273.15) / 9.80665, air_c)
    return constants


def sensor_report(index, sensor, metadata, reason, reference, ref_count, ref_unusable):
    lines = [f"\nBMP{index} channel={metadata.get(f'bmp{index}_channel', 'unknown')} "
             f"address={metadata.get(f'bmp{index}_address', 'unknown')} "
             f"ready={metadata.get(f'bmp{index}_ready', 'unknown')} nvm={reason}",
             f"  nvm_hex={metadata.get(f'bmp{index}_nvm', 'missing')}",
             f"  valid_raw_pairs={sensor.raw[0].count} sentinel_packets={sensor.sentinels} "
             f"out_of_24bit_packets={sensor.out_of_24bit} invalid_physical_packets={sensor.invalid_physical}",
             f"  changes={sensor.changes} longest_unchanged_observed_s={sensor.longest_unchanged_s:.6f}"]
    for name, metric in zip(("pressure_raw", "temperature_raw"), sensor.raw):
        lines.append(f"  {name} {metric.describe(precision=9)}")
    for name, metric in zip(("pressure_pa", "temperature_c"), sensor.physical):
        lines.append(f"  {name} {metric.describe()}")
    lines.append(f"  reference_valid_packets={ref_count} reference_unusable_packets={ref_unusable}")
    if reference is None:
        lines.append("  reference_pressure_pa=unavailable height_m=unavailable")
    else:
        pressure, coefficient, temperature = reference
        lines.append(f"  reference_pressure_pa={pressure:.9f} air_temperature_c={temperature:.6f} "
                     f"height_coefficient_m={coefficient:.6f}")
        if sensor.physical[0].values.count:
            lines.append(f"  delta_pressure_mean_pa={sensor.physical[0].values.mean - pressure:.6f}")
        lines.append(f"  height_m {sensor.height.describe()}")
    return lines


def analyze_capture(path, start_s=0.0, end_s=None, reference_s=None, rest=False,
                    air_temperature_c=None):
    window = (start_s, end_s)
    validate_window(window)
    if reference_s is not None:
        validate_window(reference_s)
        if reference_s[1] is None:
            raise ValueError("referencia deve ter inicio e fim explicitos")
    if air_temperature_c is not None and (
        not math.isfinite(air_temperature_c) or air_temperature_c <= -273.15
    ):
        raise ValueError("temperatura do ar deve ser finita e maior que -273.15 C")
    path, metadata, prefix = load_capture(Path(path))
    calibrations, reasons = calibrations_with_status(path, metadata)
    stats, timing = ParseStats(), Timing()
    sensors = [BmpMetrics(), BmpMetrics()]
    references = [[Metric(), Metric()] for _ in range(2)]
    reference_unusable = [0, 0]
    differences = [Trend(), Trend()]
    selected = 0
    first = last = None
    for time_s, packet in timed_packets(path, prefix, stats, timing):
        physical = [compensate(raw, calibration)
                    for raw, calibration in zip(packet.bmp_raw, calibrations)]
        if reference_s is not None and in_window(time_s, reference_s):
            for index, (metrics, values) in enumerate(zip(references, physical)):
                if values is not None:
                    for metric, value in zip(metrics, values):
                        metric.add(value)
                else:
                    reference_unusable[index] += 1
        if not in_window(time_s, window):
            continue
        selected += 1
        first = time_s if first is None else first
        last = time_s
        for sensor, raw, values, calibration in zip(sensors, packet.bmp_raw, physical, calibrations):
            sensor.add(raw, values, time_s, calibration)
        if all(value is not None for value in physical):
            for metric, left, right in zip(differences, physical[0], physical[1]):
                metric.add(time_s, left - right)

    damaged = any((stats.crc_failures, stats.discarded_bytes, stats.sequence_gaps,
                   timing.sequence_errors, timing.timestamp_errors))
    integrity = "failed" if damaged else "insufficient" if selected < 2 else "passed"
    constants = reference_constants(references, reference_unusable, reference_s, timing, air_temperature_c)
    if damaged:
        constants = [None, None]
    if any(value is not None for value in constants):
        for time_s, packet in timed_packets(path, prefix, ParseStats(), Timing()):
            if in_window(time_s, window):
                for sensor, raw, calibration, reference in zip(sensors, packet.bmp_raw, calibrations, constants):
                    physical = compensate(raw, calibration)
                    if physical is not None and reference is not None:
                        sensor.height.add(time_s, reference[1] * math.log(reference[0] / physical[0]))
    invalid = any(s.sentinels or s.out_of_24bit or s.invalid_physical for s in sensors)
    lines = ["evaluation=bmp report_version=1", f"input={path.resolve()}",
             f"confirmed_prefix_bytes={prefix} ignored_unconfirmed_bytes={path.stat().st_size - prefix}",
             summarize_timestamps(stats, timing.duration_us if stats.valid_packets >= 2 else None),
             f"interval_ms {timing.intervals.describe(0.001)}",
             f"sequence_duplicate_or_ambiguous={timing.sequence_errors} timestamp_inconsistencies={timing.timestamp_errors}",
             f"capture_integrity={integrity} scope=full_confirmed_prefix",
             f"sensor_data_quality={'failed' if invalid else 'raw_only' if None in calibrations else 'passed'} scope=selected_window",
             f"selected_packets={selected} selected_span_s={last - first if selected else 0:.6f}",
             f"window_start_s={start_s} window_end_s={end_s if end_s is not None else 'end'} reference_window_s={reference_s}",
             f"bmp_nominal_rate_hz={metadata.get('bmp_sample_rate_hz', 'unknown')} rest_declared={int(rest)}",
             "trend_interpretation=" + ("rest_drift" if rest else "window_trend_only"),
             "air_temperature_source=" + ("not_used" if reference_s is None else
                                           "user_supplied" if air_temperature_c is not None else "reference_sensor_estimate")]
    for index, sensor in enumerate(sensors):
        lines.extend(sensor_report(index, sensor, metadata, reasons[index], constants[index],
                                   references[index][0].count, reference_unusable[index]))
    lines.extend(("\nCOMPARISON signed_bmp0_minus_bmp1",
                  f"  pressure_difference_pa {differences[0].describe()}",
                  f"  temperature_difference_c {differences[1].describe()}"))
    status = load_session_status(path)
    lines.append(f"\nSTATUS_SNAPSHOT uptime_ms={status.get('uptime_ms', 'unknown')} "
                 f"packets_produced={status.get('packets_produced', 'unknown')}")
    lines.extend(f"  {key}={value}" for key, value in status.items() if key.startswith("bmp"))
    lines.extend(("snapshot_is_periodic_not_per_packet_or_selected_window",
                  "height_formula=R*T/g*ln(P_reference/P); R=287.05 g=9.80665 reference_is_fixed",
                  "sample_weight=packet_including_cache; changes_are_not_odr; repetitions_are_not_proof_of_stale_data",
                  "physical_suitability=inconclusive_reference_required; no_filters_or_drift_correction"))
    return "\n".join(lines), 0 if integrity == "passed" and not invalid else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="pasta da sessao SD ou imu.bin")
    parser.add_argument("--start-s", type=float, default=0.0)
    parser.add_argument("--end-s", type=float, help="fim exclusivo")
    parser.add_argument("--reference-s", type=float, nargs=2, metavar=("START", "END"))
    parser.add_argument("--air-temperature-c", type=float, help="temperatura medida do ar; senao, estimativa do sensor")
    parser.add_argument("--rest", action="store_true", help="declarar a janela selecionada como repouso")
    args = parser.parse_args()
    try:
        report, code = analyze_capture(args.input, args.start_s, args.end_s,
                                       args.reference_s, args.rest, args.air_temperature_c)
    except (OSError, ValueError) as exc:
        print(f"avaliacao_bmp: {exc}", file=sys.stderr)
        return 2
    print(report)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
