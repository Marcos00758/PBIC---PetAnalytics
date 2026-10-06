"""Independent, stdlib-only verifier for an ESP32 SDT1 run copied from SD."""
import argparse
import json
from pathlib import Path
import struct
import zlib

RECORD_BYTES = 256
DATA_MAGIC = 0x31544453
CHECKPOINT_MAGIC = 0x31434453


def mix(value):
    value &= 0xFFFFFFFF
    value = ((value ^ (value >> 16)) * 0x7FEB352D) & 0xFFFFFFFF
    value = ((value ^ (value >> 15)) * 0x846CA68B) & 0xFFFFFFFF
    return value ^ (value >> 16)


def verify_case(folder, case):
    """Validate the durable prefix; report uncommitted tails separately."""
    prefix = folder / f"c{case:03d}"
    checkpoint_path = prefix.with_suffix(".chk")
    last = None
    tail = False
    with checkpoint_path.open("rb") as source:
        while item := source.read(32):
            if len(item) != 32:
                tail = True
                break
            magic, seed, durable, planned, state, crc = struct.unpack("<IIQQII", item)
            valid = (magic == CHECKPOINT_MAGIC and zlib.crc32(item[:28]) == crc
                     and planned > 0 and planned % 256 == 0
                     and durable <= planned and durable % 256 == 0
                     and state in (0, 1) and (state != 1 or durable == planned))
            if not valid:
                tail = True
                break
            if last and (seed != last[0] or planned != last[2]
                         or durable < last[1] or last[3] == 1):
                raise ValueError(f"case {case}: checkpoint order/identity mismatch")
            last = seed, durable, planned, state
    if last is None:
        raise ValueError(f"case {case}: no valid checkpoint")
    seed, durable, planned, state = last
    if seed != 0x50424943 ^ case:
        raise ValueError(f"case {case}: checkpoint belongs to another case")
    data_path = prefix.with_suffix(".bin")
    actual = data_path.stat().st_size
    if actual < durable or (state == 1 and (actual != durable or tail)):
        raise ValueError(f"case {case}: size/checkpoint mismatch: actual={actual}, durable={durable}")
    if case in (18, 20) and durable < 65536:
        raise ValueError(f"case {case}: interruption prefix checkpoint missing")
    with data_path.open("rb") as source:
        for sequence in range(durable // RECORD_BYTES):
            record = source.read(RECORD_BYTES)
            magic, got_seed, got_sequence = struct.unpack("<IIQ", record[:16])
            if (magic, got_seed, got_sequence) != (DATA_MAGIC, seed, sequence):
                raise ValueError(f"case {case}: identity/sequence mismatch at record {sequence}")
            if zlib.crc32(record[:252]) != struct.unpack("<I", record[252:])[0]:
                raise ValueError(f"case {case}: CRC mismatch at record {sequence}")
            for index in range(16, 252):
                expected = mix(seed ^ (sequence & 0xFFFFFFFF)
                               ^ (((sequence >> 32) * 0x9E3779B9) & 0xFFFFFFFF)
                               ^ ((index * 0x85EBCA6B) & 0xFFFFFFFF)) & 0xFF
                if record[index] != expected:
                    raise ValueError(f"case {case}: payload mismatch at record {sequence}, byte {index}")
    return {"case": case, "durable_bytes": durable, "actual_bytes": actual,
            "planned_bytes": planned, "complete": bool(state),
            "checkpoint_tail": tail, "uncommitted_bytes": actual - durable, "ok": True}


def verify_run(folder):
    folder = Path(folder)
    metadata = dict(line.split("=", 1) for line in (folder / "meta.txt").read_text().splitlines()
                    if "=" in line)
    if metadata.get("format") != "SDT1" or len(metadata.get("cid", "")) != 32:
        raise ValueError("missing/invalid SDT1 metadata or card CID")
    int(metadata["cid"], 16)
    manifest = (folder / "manifest.txt").read_bytes()
    if not manifest or len(manifest) % 4:
        raise ValueError("empty/truncated manifest")
    cases = []
    for offset in range(0, len(manifest), 4):
        entry = manifest[offset:offset + 4]
        if entry[3:] != b"\n" or not entry[:3].isdigit() or int(entry[:3]) != offset // 4:
            raise ValueError("invalid case sequence in manifest")
        cases.append(int(entry[:3]))
    declared = {f"c{case:03d}{extension}" for case in cases for extension in (".bin", ".chk")}
    actual = {path.name for pattern in ("c*.bin", "c*.chk") for path in folder.glob(pattern)}
    if actual != declared:
        raise ValueError(f"manifest/file mismatch: missing={sorted(declared - actual)}, extra={sorted(actual - declared)}")
    results = []
    for case in cases:
        try:
            results.append(verify_case(folder, case))
        except (OSError, ValueError, struct.error) as error:
            results.append({"case": case, "ok": False, "error": str(error)})
    by_case = {result["case"]: result for result in results}
    required = list(range(18)) + [19]
    matrix_complete = (all(by_case.get(case, {}).get("complete", False)
                           and by_case.get(case, {}).get("ok", False) for case in required)
                       and by_case.get(18, {}).get("ok", False))
    return {"path": str(folder), "cid": metadata["cid"], "cases": results,
            "matrix_complete": matrix_complete,
            "power_cut_prefix_verified": by_case.get(20, {}).get("ok", False),
            "ok": all(case["ok"] for case in results)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path, help="Copied SDTEST/Rxxxxxx directory")
    args = parser.parse_args()
    try:
        result = verify_run(args.folder)
    except (OSError, ValueError) as error:
        result = {"ok": False, "error": str(error)}
    print(json.dumps(result, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
