import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from calibrate_magnetometer import estimate_calibration


class CalibrateMagnetometerTest(unittest.TestCase):
    def test_estimates_offsets_and_diagonal_scale(self):
        hard_iron, matrix = estimate_calibration(
            (-200, -100, -300), (400, 500, 300), minimum_axis_span_ut=10
        )
        self.assertEqual(hard_iron, (15.0, 30.0, 0.0))
        self.assertEqual(matrix, ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)))

    def test_rejects_capture_without_enough_rotation(self):
        with self.assertRaises(ValueError):
            estimate_calibration((-5, -5, -5), (5, 5, 5))


if __name__ == "__main__":
    unittest.main()
