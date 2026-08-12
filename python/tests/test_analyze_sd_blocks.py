import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from analyze_sd_blocks import analyze_status


def phase_fields(index: int, block: int, long_pauses: int = 0) -> str:
    prefix = f"phase{index}_"
    buckets = {
        "lt_1ms": 99 - long_pauses,
        "1_2ms": 0,
        "2_5ms": 0,
        "5_10ms": 0,
        "10_20ms": 0,
        "20_50ms": long_pauses,
        "50_100ms": 0,
        "ge_100ms": 0,
    }
    lines = [
        f"{prefix}write_block_bytes={block}",
        f"{prefix}bytes_written={block * 99}",
        f"{prefix}write_attempts=99",
        f"{prefix}write_failures=0",
        f"{prefix}silence_blocks_inserted=0",
        f"{prefix}gap_events=0",
        f"{prefix}max_gap_blocks=0",
        f"{prefix}max_buffered_bytes=4096",
        f"{prefix}max_write_duration_us=25000",
    ]
    lines.extend(
        f"{prefix}write_latency_{name}={count}" for name, count in buckets.items()
    )
    return "\n".join(lines)


class AnalyzeSdBlocksTest(unittest.TestCase):
    def test_recommends_smaller_acceptable_block(self):
        with tempfile.TemporaryDirectory() as directory:
            status = Path(directory) / "status.txt"
            status.write_text(
                "state=completed_duration\n"
                + phase_fields(0, 1024, 1)
                + "\n"
                + phase_fields(1, 2048, 1)
                + "\n",
                encoding="ascii",
            )

            phases, recommendation = analyze_status(status)

            self.assertEqual(recommendation, "1024")
            self.assertEqual(phases[0]["pauses_20ms"], 1)
            self.assertAlmostEqual(phases[1]["calls_per_mib"], 512.0)


if __name__ == "__main__":
    unittest.main()
