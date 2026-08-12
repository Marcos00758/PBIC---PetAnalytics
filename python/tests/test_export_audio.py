import sys
import tempfile
import unittest
import wave
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from export_audio import export_audio


class ExportAudioTest(unittest.TestCase):
    def test_exports_only_journal_prefix_with_gain(self):
        with tempfile.TemporaryDirectory() as directory:
            session = Path(directory) / "S001"
            session.mkdir()
            samples = np.array((100, -200, 300, -400), dtype="<i2")
            (session / "audio.raw").write_bytes(samples.tobytes() + bytes(2048))
            (session / "meta.txt").write_text(
                "audio_sample_rate_hz=44100\n"
                "audio_channels=1\n"
                "audio_bits_per_sample=16\n",
                encoding="ascii",
            )
            (session / "journal.txt").write_text(
                f"audio_valid_bytes={samples.nbytes}\n"
                "audio_silence_blocks_inserted=3\n"
                "audio_gap_events=2\n"
                "audio_max_gap_blocks=3\n",
                encoding="ascii",
            )
            (session / "status.txt").write_text(
                "audio_capture_blocks_dropped=2\n"
                "sd_audio_silence_blocks_inserted=2\n"
                "sd_audio_gap_events=1\n",
                encoding="ascii",
            )
            output = Path(directory) / "audio.wav"

            stats = export_audio(session, output, gain_db=6.0206)

            with wave.open(str(output), "rb") as wav:
                exported = np.frombuffer(wav.readframes(4), dtype="<i2")
            np.testing.assert_array_equal(exported, samples * 2)
            self.assertEqual(stats["valid_bytes"], samples.nbytes)
            self.assertEqual(stats["samples"], 4)
            self.assertEqual(stats["capture_blocks_dropped"], 2)
            self.assertEqual(stats["silence_blocks_inserted"], 3)
            self.assertEqual(stats["audio_gap_events"], 2)
            self.assertEqual(stats["max_audio_gap_blocks"], 3)


if __name__ == "__main__":
    unittest.main()
