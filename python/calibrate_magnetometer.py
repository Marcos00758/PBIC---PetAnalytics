"""Estimate initial hard-iron and diagonal soft-iron magnetometer calibration."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from parse_data import MAG_UT_PER_COUNT, ParseStats, iter_file_packets

DEFAULT_MINIMUM_AXIS_SPAN_UT = 20.0


def estimate_calibration(
    minimum_raw: tuple[int, int, int],
    maximum_raw: tuple[int, int, int],
    minimum_axis_span_ut: float = DEFAULT_MINIMUM_AXIS_SPAN_UT,
) -> tuple[tuple[float, ...], tuple[tuple[float, ...], ...]]:
    spans_ut = tuple(
        (maximum - minimum) * MAG_UT_PER_COUNT
        for minimum, maximum in zip(minimum_raw, maximum_raw)
    )
    if any(span < minimum_axis_span_ut for span in spans_ut):
        raise ValueError(
            "insufficient 3D rotation: every magnetic axis must span at least "
            f"{minimum_axis_span_ut:g} uT; measured spans={spans_ut}"
        )

    hard_iron_ut = tuple(
        ((minimum + maximum) / 2.0) * MAG_UT_PER_COUNT
        for minimum, maximum in zip(minimum_raw, maximum_raw)
    )
    half_spans = tuple(span / 2.0 for span in spans_ut)
    target_radius = sum(half_spans) / 3.0
    diagonal = tuple(target_radius / half_span for half_span in half_spans)
    soft_iron_matrix = (
        (diagonal[0], 0.0, 0.0),
        (0.0, diagonal[1], 0.0),
        (0.0, 0.0, diagonal[2]),
    )
    return hard_iron_ut, soft_iron_matrix


def calibrate_file(path: Path, minimum_axis_span_ut: float) -> dict[str, object]:
    stats = ParseStats()
    minimum = [[32767, 32767, 32767] for _ in range(3)]
    maximum = [[-32768, -32768, -32768] for _ in range(3)]

    for parsed in iter_file_packets(path, stats):
        for sensor in range(3):
            base = sensor * 9 + 6
            for axis, value in enumerate(parsed.packet.values[base : base + 3]):
                minimum[sensor][axis] = min(minimum[sensor][axis], value)
                maximum[sensor][axis] = max(maximum[sensor][axis], value)

    if stats.valid_packets == 0:
        raise ValueError("capture contains no valid packets")

    document: dict[str, object] = {
        "source_file": str(path),
        "packet_count": stats.valid_packets,
        "method": "axis_minmax_diagonal_soft_iron",
    }
    for sensor in range(3):
        minimum_tuple = tuple(minimum[sensor])
        maximum_tuple = tuple(maximum[sensor])
        hard_iron, matrix = estimate_calibration(
            minimum_tuple, maximum_tuple, minimum_axis_span_ut
        )
        document[f"icm{sensor}"] = {
            "hard_iron_ut": hard_iron,
            "soft_iron_matrix": matrix,
            "raw_min": minimum_tuple,
            "raw_max": maximum_tuple,
        }
    return document


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="capture made during slow 3D rotation")
    parser.add_argument("--output", type=Path, help="calibration JSON output path")
    parser.add_argument(
        "--minimum-axis-span-ut",
        type=float,
        default=DEFAULT_MINIMUM_AXIS_SPAN_UT,
        help="minimum required span on every axis (default: 20 uT)",
    )
    args = parser.parse_args()
    if args.minimum_axis_span_ut <= 0:
        parser.error("--minimum-axis-span-ut must be positive")

    output = args.output or args.input.with_name(
        f"{args.input.stem}_mag_calibration.json"
    )
    try:
        document = calibrate_file(args.input, args.minimum_axis_span_ut)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc
    print(f"calibration_file={output.resolve()}")


if __name__ == "__main__":
    main()
