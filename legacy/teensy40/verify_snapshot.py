"""Verify the preserved working tree and experimental archive without modifying them."""

import hashlib
import json
from pathlib import Path
import zipfile


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    base = Path(__file__).resolve().parent
    manifest = json.loads((base / "snapshot-manifest.json").read_text(encoding="utf-8"))
    for record in manifest["files"] + [manifest["local_changes_patch"]]:
        path = base / record["path"]
        if sha256(path) != record["sha256"]:
            raise SystemExit(f"SHA-256 mismatch: {record['path']}")
    data = manifest["experimental_data"]
    archive = base / data["archive"]
    if not archive.is_file():
        raise SystemExit(f"Experimental archive missing (ignored by Git): {archive}")
    if sha256(archive) != data["archive_sha256"]:
        raise SystemExit("SHA-256 mismatch: experimental archive")
    with zipfile.ZipFile(archive) as restored:
        if set(restored.namelist()) != {r["path"] for r in data["files"]}:
            raise SystemExit("Experimental archive has unexpected or missing entries")
        for record in data["files"]:
            with restored.open(record["path"]) as stream:
                actual = hashlib.file_digest(stream, "sha256").hexdigest()
            if actual != record["sha256"]:
                raise SystemExit(f"SHA-256 mismatch: {record['path']}")
    print(f"Verified: {len(manifest['files'])} project files, local changes patch, "
          f"{len(data['files'])} experimental files ({data['original_total_bytes']} bytes).")


if __name__ == "__main__":
    main()
