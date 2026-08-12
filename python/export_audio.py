"""Export the valid PCM prefix of an SD session to a playable WAV file."""

from __future__ import annotations

import argparse
import math
import wave
from pathlib import Path

import numpy as np

from parse_data import (
    load_session_journal,
    load_session_metadata,
    load_session_status,
    valid_audio_bytes,
)

DEFAULT_CHUNK_BYTES = 64 * 1024


def export_audio(
    input_path: Path,
    output_path: Path,
    gain_db: float = 0.0,
) -> dict[str, float | int]:
    audio_path = input_path / "audio.raw" if input_path.is_dir() else input_path
    metadata = load_session_metadata(audio_path)
    journal = load_session_journal(audio_path)
    status = load_session_status(audio_path)
    sample_rate = int(metadata.get("audio_sample_rate_hz", "44100"))
    channels = int(metadata.get("audio_channels", "1"))
    bits = int(metadata.get("audio_bits_per_sample", "16"))
    if channels != 1 or bits != 16:
        raise ValueError("only mono PCM signed 16-bit sessions are supported")

    valid_bytes = valid_audio_bytes(audio_path)
    valid_bytes -= valid_bytes % 2
    gain = 10.0 ** (gain_db / 20.0)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    samples = 0
    sum_squares = 0.0
    input_peak = 0
    output_clipped = 0
    remaining = valid_bytes
    with audio_path.open("rb") as source, wave.open(str(output_path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        while remaining > 0:
            raw = source.read(min(DEFAULT_CHUNK_BYTES, remaining))
            if not raw:
                break
            remaining -= len(raw)
            pcm = np.frombuffer(raw, dtype="<i2").astype(np.float64)
            if pcm.size == 0:
                continue
            samples += int(pcm.size)
            sum_squares += float(np.dot(pcm, pcm))
            input_peak = max(input_peak, int(np.max(np.abs(pcm))))
            amplified = pcm * gain
            output_clipped += int(np.count_nonzero(np.abs(amplified) > 32767.0))
            encoded = np.clip(np.rint(amplified), -32768, 32767).astype("<i2")
            wav.writeframesraw(encoded.tobytes())

    rms = math.sqrt(sum_squares / samples) if samples else 0.0
    rms_dbfs = 20.0 * math.log10(rms / 32768.0) if rms else -120.0
    safe_gain_db = (
        20.0 * math.log10(32767.0 / input_peak) if input_peak else 120.0
    )
    return {
        "valid_bytes": valid_bytes - remaining,
        "samples": samples,
        "duration_s": samples / sample_rate if sample_rate else 0.0,
        "input_peak": input_peak,
        "input_rms_dbfs": rms_dbfs,
        "peak_safe_gain_db": safe_gain_db,
        "gain_db": gain_db,
        "output_clipped_samples": output_clipped,
        "capture_blocks_dropped": int(
            status.get("audio_capture_blocks_dropped", "0")
        ),
        "silence_blocks_inserted": int(
            journal.get(
                "audio_silence_blocks_inserted",
                status.get("sd_audio_silence_blocks_inserted", "0"),
            )
        ),
        "audio_gap_events": int(
            journal.get(
                "audio_gap_events", status.get("sd_audio_gap_events", "0")
            )
        ),
        "max_audio_gap_blocks": int(
            journal.get(
                "audio_max_gap_blocks",
                status.get("sd_audio_max_gap_blocks", "0"),
            )
        ),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="session folder or audio.raw")
    parser.add_argument("--output", type=Path, help="destination WAV path")
    parser.add_argument(
        "--gain-db",
        type=float,
        default=0.0,
        help="playback-only gain applied to the WAV copy (default: 0)",
    )
    args = parser.parse_args()

    session = args.input if args.input.is_dir() else args.input.parent
    output = args.output or Path("data") / f"{session.name}_audio.wav"
    stats = export_audio(args.input, output, args.gain_db)
    for key, value in stats.items():
        if isinstance(value, float):
            print(f"{key}={value:.3f}")
        else:
            print(f"{key}={value}")
    if stats["output_clipped_samples"]:
        print(
            "warning=output_clipped; reduce --gain-db to no more than "
            f"{stats['peak_safe_gain_db']:.3f} dB"
        )
    print(f"wav_file={output.resolve()}")


if __name__ == "__main__":
    main()
