import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("verify_sd_test", ROOT / "python/verify_sd_test.py")
verifier = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verifier)


class SdTestVerifierTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("g++ required for independent C++/Python format checks")
        cls.build = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.build.name) / "format_test.exe"
        subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "test/sd_test_format_host.cpp"), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        subprocess.run([str(self.exe), str(self.folder)], check=True)
        (self.folder / "meta.txt").write_text("format=SDT1\ncid=" + "00" * 16 + "\n")
        (self.folder / "manifest.txt").write_bytes(b"000\n")

    def result(self):
        return verifier.verify_run(self.folder)

    def checkpoint(self, durable, planned, state):
        content = struct.pack("<IIQQI", verifier.CHECKPOINT_MAGIC, 0x50424943, durable, planned, state)
        (self.folder / "c000.chk").write_bytes(content + struct.pack("<I", zlib.crc32(content)))

    def test_cpp_pattern_and_checkpoint_match_independent_python(self):
        self.assertTrue(self.result()["ok"])

    def test_payload_corruption(self):
        name = self.folder / "c000.bin"
        data = bytearray(name.read_bytes()); data[87] ^= 1; name.write_bytes(data)
        self.assertFalse(self.result()["ok"])

    def test_recomputed_crc_does_not_hide_pattern_corruption(self):
        name = self.folder / "c000.bin"
        data = bytearray(name.read_bytes()); data[87] ^= 1
        struct.pack_into("<I", data, 252, zlib.crc32(data[:252])); name.write_bytes(data)
        self.assertIn("payload", self.result()["cases"][0]["error"])

    def test_reordered_records(self):
        name = self.folder / "c000.bin"; data = name.read_bytes()
        name.write_bytes(data[256:] + data[:256]); self.assertFalse(self.result()["ok"])

    def test_missing_data_is_detected_by_manifest(self):
        (self.folder / "c000.bin").unlink()
        with self.assertRaisesRegex(ValueError, "manifest/file"):
            self.result()

    def test_truncated_completed_file(self):
        name = self.folder / "c000.bin"; name.write_bytes(name.read_bytes()[:-1])
        self.assertFalse(self.result()["ok"])

    def test_preallocated_tail_is_excluded_only_for_incomplete_case(self):
        name = self.folder / "c000.bin"; name.write_bytes(name.read_bytes() + b"garbage")
        self.assertFalse(self.result()["ok"])
        self.checkpoint(512, 1024, 0)
        result = self.result(); self.assertTrue(result["ok"])
        self.assertEqual(result["cases"][0]["uncommitted_bytes"], 7)

    def test_torn_checkpoint_uses_last_valid_prefix(self):
        self.checkpoint(256, 512, 0)
        with (self.folder / "c000.chk").open("ab") as out: out.write(b"torn")
        result = self.result(); self.assertTrue(result["ok"])
        self.assertTrue(result["cases"][0]["checkpoint_tail"])

    def test_complete_checkpoint_with_tail_is_rejected(self):
        with (self.folder / "c000.chk").open("ab") as out: out.write(b"torn")
        self.assertFalse(self.result()["ok"])

    def test_invalid_checkpoint_is_rejected(self):
        (self.folder / "c000.chk").write_bytes(b"\0" * 32)
        self.assertFalse(self.result()["ok"])

    def test_truncated_manifest(self):
        (self.folder / "manifest.txt").write_bytes(b"000")
        with self.assertRaisesRegex(ValueError, "manifest"):
            self.result()

    def test_checkpoint_regression(self):
        self.checkpoint(512, 1024, 0)
        content = struct.pack("<IIQQI", verifier.CHECKPOINT_MAGIC, 0x50424943, 256, 1024, 0)
        with (self.folder / "c000.chk").open("ab") as out:
            out.write(content + struct.pack("<I", zlib.crc32(content)))
        self.assertFalse(self.result()["ok"])

    def test_corrupted_payload_inside_durable_prefix_of_incomplete_file(self):
        self.checkpoint(256, 512, 0)
        name = self.folder / "c000.bin"
        data = bytearray(name.read_bytes()); data[30] ^= 1; name.write_bytes(data)
        self.assertFalse(self.result()["ok"])

    def test_durable_prefix_does_not_imply_completed_matrix(self):
        result = self.result()
        self.assertTrue(result["ok"])
        self.assertFalse(result["matrix_complete"])

    def test_checkpoint_cannot_be_moved_between_cases(self):
        name = self.folder / "c000.chk"
        data = bytearray(name.read_bytes()); struct.pack_into("<I", data, 4, 0x50424943 ^ 1)
        struct.pack_into("<I", data, 28, zlib.crc32(data[:28])); name.write_bytes(data)
        self.assertIn("another case", self.result()["cases"][0]["error"])


if __name__ == "__main__":
    unittest.main()
