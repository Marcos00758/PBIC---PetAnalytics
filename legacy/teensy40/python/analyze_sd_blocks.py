"""Compare SD write phases and inspect each phase for PCM corruption."""

from __future__ import annotations

import argparse
import math
from pathlib import Path

import numpy as np

from parse_data import load_key_value_file


BUCKET_NAMES = (
    "lt_1ms",
    "1_2ms",
    "2_5ms",
    "5_10ms",
    "10_20ms",
    "20_50ms",
    "50_100ms",
    "ge_100ms",
)


def _integer(values: dict[str, str], key: str) -> int:
    try:
        return int(values[key])
    except KeyError as error:
        raise ValueError(f"missing benchmark field: {key}") from error
    except ValueError as error:
        raise ValueError(f"invalid integer in benchmark field: {key}") from error


def analyze_phase(values: dict[str, str], index: int) -> dict[str, int | float]:
    prefix = f"phase{index}_"
    buckets = {
        name: _integer(values, f"{prefix}write_latency_{name}")
        for name in BUCKET_NAMES
    }
    attempts = _integer(values, f"{prefix}write_attempts")
    histogram_total = sum(buckets.values())
    if histogram_total != attempts:
        raise ValueError(
            f"phase {index} histogram has {histogram_total} samples, "
            f"but write_attempts is {attempts}"
        )
    bytes_written = _integer(values, f"{prefix}bytes_written")
    pauses_20ms = (
        buckets["20_50ms"] + buckets["50_100ms"] + buckets["ge_100ms"]
    )
    pauses_50ms = buckets["50_100ms"] + buckets["ge_100ms"]
    return {
        "block_bytes": _integer(values, f"{prefix}write_block_bytes"),
        "bytes_written": bytes_written,
        "write_attempts": attempts,
        "write_failures": _integer(values, f"{prefix}write_failures"),
        "silence_blocks": _integer(values, f"{prefix}silence_blocks_inserted"),
        "gap_events": _integer(values, f"{prefix}gap_events"),
        "max_gap_blocks": _integer(values, f"{prefix}max_gap_blocks"),
        "max_buffered_bytes": _integer(values, f"{prefix}max_buffered_bytes"),
        "max_write_us": _integer(values, f"{prefix}max_write_duration_us"),
        "calls_per_mib": attempts * 1048576.0 / bytes_written if bytes_written else 0.0,
        "pauses_20ms": pauses_20ms,
        "pauses_20ms_percent": pauses_20ms * 100.0 / attempts if attempts else 0.0,
        "pauses_50ms": pauses_50ms,
        "pauses_50ms_percent": pauses_50ms * 100.0 / attempts if attempts else 0.0,
        **{f"bucket_{name}": count for name, count in buckets.items()},
    }


def analyze_pcm(audio_path: Path, phases: list[dict[str, int | float]]) -> None:
    offset = 0
    with audio_path.open("rb") as audio:
        for phase in phases:
            remaining = int(phase["bytes_written"])
            samples = 0
            sum_squares = 0.0
            peak = 0
            high_amplitude = 0
            near_high_bit_rails = 0
            audio.seek(offset)
            while remaining > 0:
                raw = audio.read(min(64 * 1024, remaining))
                if not raw:
                    raise ValueError("audio.raw ends before the recorded phase sizes")
                remaining -= len(raw)
                raw = raw[: len(raw) - len(raw) % 2]
                pcm = np.frombuffer(raw, dtype="<i2").astype(np.int32)
                absolute = np.abs(pcm)
                samples += int(pcm.size)
                sum_squares += float(np.dot(pcm.astype(np.float64), pcm))
                peak = max(peak, int(np.max(absolute, initial=0)))
                high_amplitude += int(np.count_nonzero(absolute >= 12000))
                rail_distance = np.minimum(
                    np.abs(absolute - 16384), np.abs(absolute - 32768)
                )
                near_high_bit_rails += int(
                    np.count_nonzero((absolute >= 12000) & (rail_distance <= 512))
                )
            offset += int(phase["bytes_written"])
            rms = math.sqrt(sum_squares / samples) if samples else 0.0
            rail_percent = (
                near_high_bit_rails * 100.0 / samples if samples else 0.0
            )
            phase.update(
                {
                    "pcm_samples": samples,
                    "pcm_rms_counts": rms,
                    "pcm_rms_dbfs": (
                        20.0 * math.log10(rms / 32768.0) if rms else -120.0
                    ),
                    "pcm_peak": peak,
                    "pcm_high_amplitude_samples": high_amplitude,
                    "pcm_near_high_bit_rails": near_high_bit_rails,
                    "pcm_near_high_bit_rails_percent": rail_percent,
                    "pcm_suspicious": rail_percent >= 0.05,
                }
            )


def recommend(phases: list[dict[str, int | float | bool]]) -> str:
    acceptable = [
        phase
        for phase in phases
        if phase["write_failures"] == 0
        and phase["gap_events"] == 0
        and phase["bucket_ge_100ms"] == 0
        and not phase.get("pcm_suspicious", False)
    ]
    if not acceptable:
        return "inconclusive_no_block_without_failures_gaps_or_100ms_pauses"
    if len(acceptable) == 1:
        return str(acceptable[0]["block_bytes"])

    smaller, larger = sorted(acceptable, key=lambda phase: phase["block_bytes"])
    smaller_long = float(smaller["pauses_20ms_percent"])
    larger_long = float(larger["pauses_20ms_percent"])
    materially_worse = (
        smaller_long > larger_long * 1.25
        and smaller_long - larger_long > 0.1
    )
    return str(larger["block_bytes"] if materially_worse else smaller["block_bytes"])


def analyze_status(
    status_path: Path, inspect_pcm: bool = True
) -> tuple[list[dict[str, int | float | bool]], str]:
    values = load_key_value_file(status_path)
    if values.get("state") != "completed_duration":
        raise ValueError("benchmark status is not completed_duration")
    phases = [analyze_phase(values, index) for index in range(2)]
    audio_path = status_path.parent / "audio.raw"
    if inspect_pcm and audio_path.exists():
        analyze_pcm(audio_path, phases)
    return phases, recommend(phases)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path, help="/Mxxx folder or status.txt")
    args = parser.parse_args()
    status_path = args.session / "status.txt" if args.session.is_dir() else args.session
    try:
        phases, recommendation = analyze_status(status_path)
    except (OSError, ValueError) as error:
        print(f"analysis_failed: {error}")
        return 1

    for index, phase in enumerate(phases):
        print(
            f"phase={index} block_bytes={phase['block_bytes']} "
            f"bytes={phase['bytes_written']} attempts={phase['write_attempts']} "
            f"calls_per_mib={phase['calls_per_mib']:.2f} "
            f"failures={phase['write_failures']} gaps={phase['gap_events']} "
            f"silence_blocks={phase['silence_blocks']} "
            f"max_buffer_bytes={phase['max_buffered_bytes']} "
            f"max_write_ms={phase['max_write_us'] / 1000.0:.3f} "
            f"pauses_ge_20ms={phase['pauses_20ms']} "
            f"pauses_ge_20ms_percent={phase['pauses_20ms_percent']:.3f} "
            f"pauses_ge_50ms={phase['pauses_50ms']}"
        )
        print(
            "latency_histogram "
            + " ".join(
                f"{name}={phase[f'bucket_{name}']}" for name in BUCKET_NAMES
            )
        )
        if "pcm_samples" in phase:
            print(
                f"pcm rms_counts={phase['pcm_rms_counts']:.2f} "
                f"rms_dbfs={phase['pcm_rms_dbfs']:.3f} "
                f"peak={phase['pcm_peak']} "
                f"samples_abs_ge_12000={phase['pcm_high_amplitude_samples']} "
                f"near_high_bit_rails={phase['pcm_near_high_bit_rails']} "
                f"near_high_bit_rails_percent="
                f"{phase['pcm_near_high_bit_rails_percent']:.4f} "
                f"suspicious={int(bool(phase['pcm_suspicious']))}"
            )
    print(f"provisional_recommended_block_bytes={recommendation}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
