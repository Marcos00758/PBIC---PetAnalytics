"""Capture an exact sensor-time window from the PBIC USB binary stream."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError as exc:
    raise SystemExit(
        "pyserial is required; install it with: pip install -r python/requirements.txt"
    ) from exc

from parse_data import PACKET_SIZE, PACKET_VERSION, elapsed_us, parse_stream, summarize

DEFAULT_DURATION_SECONDS = 10.0
SERIAL_BAUD = 115200
TEENSY_USB_VID = 0x16C0
READ_TIMEOUT_SECONDS = 0.05
SYNC_TIMEOUT_SECONDS = 15.0
CAPTURE_TIMEOUT_MARGIN_SECONDS = 5.0


def detect_port() -> str:
    ports = list(list_ports.comports())
    teensy_ports = [
        port
        for port in ports
        if port.vid == TEENSY_USB_VID
        or "teensy" in (port.description or "").lower()
        or "teensy" in (port.manufacturer or "").lower()
    ]
    if len(teensy_ports) == 1:
        return teensy_ports[0].device
    if not teensy_ports and len(ports) == 1:
        return ports[0].device

    available = ", ".join(
        f"{port.device} ({port.description or 'unknown'})" for port in ports
    ) or "none"
    if len(teensy_ports) > 1:
        raise RuntimeError(f"multiple Teensy ports found; use --port. Available: {available}")
    raise RuntimeError(f"Teensy port not detected; use --port. Available: {available}")


def read_available(port: serial.Serial) -> bytes:
    return port.read(max(port.in_waiting, 1))


def synchronize(port: serial.Serial) -> bytearray:
    deadline = time.monotonic() + SYNC_TIMEOUT_SECONDS
    pending = bytearray()
    received_bytes = 0
    while time.monotonic() < deadline:
        received = read_available(port)
        received_bytes += len(received)
        pending.extend(received)
        packets, _ = parse_stream(pending)
        if packets:
            return bytearray(pending[packets[0].offset :])
        if len(pending) > PACKET_SIZE * 4:
            del pending[: -PACKET_SIZE]
    raise TimeoutError(
        "no valid sensor packet received before synchronization timeout; "
        f"received_bytes={received_bytes} expected_packet_size={PACKET_SIZE}"
    )


def capture_sensor_window(
    port: serial.Serial, duration_seconds: float
) -> tuple[bytes, int]:
    raw = synchronize(port)
    first_packets, _ = parse_stream(raw)
    first_timestamp = first_packets[0].packet.timestamp_us
    duration_us = round(duration_seconds * 1_000_000)
    host_deadline = time.monotonic() + duration_seconds + CAPTURE_TIMEOUT_MARGIN_SECONDS

    while time.monotonic() < host_deadline:
        packets, _ = parse_stream(raw)
        for parsed in packets[1:]:
            if elapsed_us(first_timestamp, parsed.packet.timestamp_us) >= duration_us:
                return bytes(raw[: parsed.offset]), first_timestamp
        raw.extend(read_available(port))

    raise TimeoutError("capture did not reach the requested sensor-time duration")


def default_output_path() -> Path:
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    return Path("data") / f"imu_10s_{timestamp}.bin"


def write_capture(
    output: Path, raw: bytes, port_name: str, duration_seconds: float
) -> Path:
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(raw)

    packets, stats = parse_stream(raw)
    metadata = {
        "captured_at_utc": datetime.now(timezone.utc).isoformat(),
        "port": port_name,
        "requested_duration_s": duration_seconds,
        "packet_version": PACKET_VERSION,
        "packet_size": PACKET_SIZE,
        "raw_bytes": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "valid_packets": stats.valid_packets,
        "crc_failures": stats.crc_failures,
        "discarded_bytes": stats.discarded_bytes,
        "trailing_bytes": stats.trailing_bytes,
        "sequence_gaps": stats.sequence_gaps,
    }
    if packets:
        metadata["first_timestamp_us"] = packets[0].packet.timestamp_us
        metadata["last_timestamp_us"] = packets[-1].packet.timestamp_us
    metadata_path = output.with_suffix(output.suffix + ".json")
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return metadata_path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial port, for example COM3; auto-detected if omitted")
    parser.add_argument("--output", type=Path, default=None, help="output .bin path")
    parser.add_argument(
        "--duration",
        type=float,
        default=DEFAULT_DURATION_SECONDS,
        help="sensor-time capture duration in seconds (default: 10)",
    )
    args = parser.parse_args()
    if args.duration <= 0 or args.duration >= 1800:
        parser.error("--duration must be greater than 0 and less than 1800 seconds")

    try:
        port_name = args.port or detect_port()
        output = args.output or default_output_path()
        print(f"port={port_name}")
        print(f"waiting_for_valid_packet duration_s={args.duration:g}")
        with serial.Serial(port_name, SERIAL_BAUD, timeout=READ_TIMEOUT_SECONDS) as port:
            port.dtr = True
            port.rts = True
            time.sleep(0.2)
            port.reset_input_buffer()
            raw, _ = capture_sensor_window(port, args.duration)
        metadata_path = write_capture(output, raw, port_name, args.duration)
    except (OSError, RuntimeError, TimeoutError, serial.SerialException) as exc:
        print(f"capture_failed: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc

    packets, stats = parse_stream(raw)
    print(summarize(packets, stats))
    print(f"raw_file={output.resolve()}")
    print(f"metadata_file={metadata_path.resolve()}")


if __name__ == "__main__":
    main()
