"""Estimate initial hard-iron and diagonal soft-iron magnetometer calibration."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from parse_data import (
    MAG_UT_PER_COUNT,
    ImuPacket,
    ParseStats,
    elapsed_us,
    iter_file_packets,
    load_session_metadata,
)

DEFAULT_MINIMUM_AXIS_SPAN_UT = 20.0
DEFAULT_LIVE_DURATION_SECONDS = 60.0
SENSOR_COUNT = 3


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


class AxisExtremes:
    """Running minimum and maximum of the nine raw magnetic axes."""

    def __init__(self) -> None:
        self.minimum = [[32767, 32767, 32767] for _ in range(SENSOR_COUNT)]
        self.maximum = [[-32768, -32768, -32768] for _ in range(SENSOR_COUNT)]
        self.packets = 0

    def add(self, packet: ImuPacket) -> None:
        for sensor in range(SENSOR_COUNT):
            base = sensor * 9 + 6
            for axis, value in enumerate(packet.values[base : base + 3]):
                if value < self.minimum[sensor][axis]:
                    self.minimum[sensor][axis] = value
                if value > self.maximum[sensor][axis]:
                    self.maximum[sensor][axis] = value
        self.packets += 1

    def coverage_ut(self) -> tuple[tuple[float, ...], ...]:
        """Span reached on every axis, the progress a rotation has to fill."""
        return tuple(
            tuple(
                (self.maximum[sensor][axis] - self.minimum[sensor][axis])
                * MAG_UT_PER_COUNT
                for axis in range(3)
            )
            for sensor in range(SENSOR_COUNT)
        )


def build_document(
    extremes: AxisExtremes,
    minimum_axis_span_ut: float,
    source: dict[str, object],
    channels: dict[int, str] | None = None,
) -> dict[str, object]:
    if extremes.packets == 0:
        raise ValueError("capture contains no valid packets")

    document: dict[str, object] = dict(source)
    document["packet_count"] = extremes.packets
    document["method"] = "axis_minmax_diagonal_soft_iron"

    for sensor in range(SENSOR_COUNT):
        minimum_tuple = tuple(extremes.minimum[sensor])
        maximum_tuple = tuple(extremes.maximum[sensor])
        hard_iron, matrix = estimate_calibration(
            minimum_tuple, maximum_tuple, minimum_axis_span_ut
        )
        entry: dict[str, object] = {
            "hard_iron_ut": hard_iron,
            "soft_iron_matrix": matrix,
            "raw_min": minimum_tuple,
            "raw_max": maximum_tuple,
        }
        # The multiplexer channel is recorded so a calibration can never be
        # applied to a sensor it was not measured on. The channel assignment has
        # changed once already, which silently mismatched an older file.
        if channels and sensor in channels:
            entry["mux_channel"] = channels[sensor]
        document[f"icm{sensor}"] = entry
    return document


def session_channels(path: Path) -> dict[int, str]:
    metadata = load_session_metadata(path)
    channels: dict[int, str] = {}
    for sensor in range(SENSOR_COUNT):
        value = metadata.get(f"icm{sensor}_channel")
        if value is not None:
            channels[sensor] = value
    return channels


def calibrate_live(
    port_name: str | None,
    duration_seconds: float,
    minimum_axis_span_ut: float,
) -> dict[str, object]:
    """Collect a rotation straight from the presentation stream."""
    import time

    import serial

    from capture_serial import READ_TIMEOUT_SECONDS, SERIAL_BAUD, detect_port
    from presentation_monitor import (
        BANNER_TIMEOUT_SECONDS,
        SerialSource,
        StreamDecoder,
        wait_for_banner,
    )

    decoder = StreamDecoder()
    extremes = AxisExtremes()
    resolved = port_name or detect_port()
    print(f"port={resolved}")

    with serial.Serial(resolved, SERIAL_BAUD, timeout=READ_TIMEOUT_SECONDS) as port:
        port.dtr = True
        port.rts = True
        time.sleep(0.2)
        port.reset_input_buffer()
        source = SerialSource(port)
        opening: list[ImuPacket] = []
        if not wait_for_banner(source, decoder, BANNER_TIMEOUT_SECONDS, opening):
            raise TimeoutError(
                "no firmware banner received; confirm that "
                "kPresentationStreamEnabled is true in src/config/constants.h"
            )
        if not decoder.device.streaming:
            raise RuntimeError(
                "the firmware reported streaming=0; run "
                "python/presentation_monitor.py for the sensor report"
            )

        print(f"rotate_slowly_in_every_direction duration_s={duration_seconds:g}")
        duration_us = round(duration_seconds * 1_000_000)
        elapsed = 0
        previous: int | None = None
        reported = -1.0
        deadline = time.monotonic() + duration_seconds * 3 + 30.0

        for packet in opening:
            if previous is not None:
                elapsed += elapsed_us(previous, packet.timestamp_us)
            previous = packet.timestamp_us
            extremes.add(packet)

        while elapsed < duration_us:
            if time.monotonic() > deadline:
                raise TimeoutError(
                    "the sensor stream did not reach the requested duration; "
                    f"received_packets={extremes.packets}"
                )
            for packet in decoder.feed(source.read()):
                if previous is not None:
                    elapsed += elapsed_us(previous, packet.timestamp_us)
                previous = packet.timestamp_us
                extremes.add(packet)
            seconds = elapsed / 1_000_000
            if seconds - reported >= 5.0:
                reported = seconds
                worst = min(min(axes) for axes in extremes.coverage_ut())
                print(
                    f"elapsed_s={seconds:0.0f} "
                    f"smallest_axis_span_ut={worst:0.1f} "
                    f"target_ut={minimum_axis_span_ut:g}"
                )

    channels = {
        sensor: values["channel"]
        for sensor, values in decoder.device.icms.items()
        if "channel" in values
    }
    source_info: dict[str, object] = {
        "source": "live_presentation_stream",
        "port": resolved,
        "firmware_version": decoder.device.header.get("firmware_version", "unknown"),
        "requested_duration_s": duration_seconds,
    }
    return build_document(extremes, minimum_axis_span_ut, source_info, channels)


def calibrate_file(path: Path, minimum_axis_span_ut: float) -> dict[str, object]:
    stats = ParseStats()
    extremes = AxisExtremes()
    for parsed in iter_file_packets(path, stats):
        extremes.add(parsed.packet)
    return build_document(
        extremes,
        minimum_axis_span_ut,
        {"source": "file", "source_file": str(path)},
        session_channels(path),
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "input",
        type=Path,
        nargs="?",
        help="capture made during slow 3D rotation; omit when using --live",
    )
    parser.add_argument(
        "--live",
        action="store_true",
        help="record the rotation directly from the presentation stream",
    )
    parser.add_argument(
        "--port", help="serial port for --live; auto-detected if omitted"
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=DEFAULT_LIVE_DURATION_SECONDS,
        help="sensor-time rotation window for --live (default: 60)",
    )
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
    if args.live == (args.input is not None):
        parser.error("give either a capture file or --live, not both")
    if args.live and (args.duration <= 0 or args.duration > 600):
        parser.error("--duration must be greater than 0 and at most 600 seconds")

    if args.live:
        output = args.output or Path("data") / "mag_calibration.json"
    else:
        output = args.output or args.input.with_name(
            f"{args.input.stem}_mag_calibration.json"
        )

    try:
        if args.live:
            document = calibrate_live(
                args.port, args.duration, args.minimum_axis_span_ut
            )
        else:
            document = calibrate_file(args.input, args.minimum_axis_span_ut)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    except (OSError, RuntimeError, TimeoutError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc

    for sensor in range(SENSOR_COUNT):
        entry = document[f"icm{sensor}"]
        offsets = ", ".join(f"{value:+0.1f}" for value in entry["hard_iron_ut"])
        channel = entry.get("mux_channel", "?")
        print(f"icm{sensor} channel={channel} hard_iron_ut={offsets}")
    print(f"calibration_file={output.resolve()}")


if __name__ == "__main__":
    main()
