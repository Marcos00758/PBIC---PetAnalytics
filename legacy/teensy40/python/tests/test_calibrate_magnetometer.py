import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from calibrate_magnetometer import (
    AxisExtremes,
    build_document,
    estimate_calibration,
)
from parse_data import MAG_UT_PER_COUNT
from presentation_panel import load_magnetometer_calibrations
from test_presentation_monitor import StreamDecoder, healthy_bmp, make_packet


def rotation_stream(count: int = 400, amplitude: int = 400) -> bytes:
    """A capture that sweeps every magnetic axis wide enough to calibrate."""
    import math

    packets = []
    for index in range(count):
        angle = 2.0 * math.pi * index / count
        values = []
        for sensor in range(3):
            offset = 50 * sensor  # a different hard iron on each sensor
            values.extend((0, 0, 4096))
            values.extend((0, 0, 0))
            values.extend(
                (
                    offset + int(amplitude * math.cos(angle)),
                    offset + int(amplitude * math.sin(angle)),
                    offset + int(amplitude * math.cos(angle + 1.0)),
                )
            )
        packets.append(
            make_packet(index * 10_000, index, tuple(values), healthy_bmp(index))
        )
    return b"".join(packets)


class EstimateCalibrationTest(unittest.TestCase):
    def test_estimates_offsets_and_diagonal_scale(self):
        hard_iron, matrix = estimate_calibration(
            (-200, -100, -300), (400, 500, 300), minimum_axis_span_ut=10
        )
        self.assertEqual(hard_iron, (15.0, 30.0, 0.0))
        self.assertEqual(matrix, ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)))

    def test_rejects_capture_without_enough_rotation(self):
        with self.assertRaises(ValueError):
            estimate_calibration((-5, -5, -5), (5, 5, 5))


class AxisExtremesTest(unittest.TestCase):
    def test_tracks_the_extremes_of_every_axis(self):
        extremes = AxisExtremes()
        for packet in StreamDecoder().feed(rotation_stream()):
            extremes.add(packet)

        self.assertEqual(extremes.packets, 400)
        for sensor in range(3):
            for axis in range(3):
                span = extremes.maximum[sensor][axis] - extremes.minimum[sensor][axis]
                self.assertGreater(span, 700)

    def test_coverage_reports_the_span_in_microtesla(self):
        extremes = AxisExtremes()
        for packet in StreamDecoder().feed(rotation_stream()):
            extremes.add(packet)
        coverage = extremes.coverage_ut()
        self.assertEqual(len(coverage), 3)
        self.assertAlmostEqual(
            coverage[0][0],
            (extremes.maximum[0][0] - extremes.minimum[0][0]) * MAG_UT_PER_COUNT,
        )

    def test_an_empty_capture_is_refused(self):
        with self.assertRaises(ValueError):
            build_document(AxisExtremes(), 20.0, {"source": "file"})


class BuildDocumentTest(unittest.TestCase):
    def setUp(self):
        self.extremes = AxisExtremes()
        for packet in StreamDecoder().feed(rotation_stream()):
            self.extremes.add(packet)

    def test_records_one_entry_per_sensor(self):
        document = build_document(self.extremes, 20.0, {"source": "file"})
        self.assertEqual(document["packet_count"], 400)
        for sensor in range(3):
            entry = document[f"icm{sensor}"]
            self.assertEqual(len(entry["hard_iron_ut"]), 3)
            self.assertEqual(len(entry["soft_iron_matrix"]), 3)

    def test_recovers_the_offset_that_was_injected(self):
        document = build_document(self.extremes, 20.0, {"source": "file"})
        # Each sensor carried a different constant offset in raw counts.
        for sensor in range(3):
            expected = 50 * sensor * MAG_UT_PER_COUNT
            self.assertAlmostEqual(
                document[f"icm{sensor}"]["hard_iron_ut"][0], expected, delta=0.2
            )

    def test_records_the_channel_when_it_is_known(self):
        document = build_document(
            self.extremes, 20.0, {"source": "live"}, {0: "4", 1: "5", 2: "6"}
        )
        self.assertEqual(document["icm2"]["mux_channel"], "6")

    def test_omits_the_channel_when_it_is_unknown(self):
        document = build_document(self.extremes, 20.0, {"source": "file"})
        self.assertNotIn("mux_channel", document["icm0"])


class ChannelGuardTest(unittest.TestCase):
    """A calibration must never be applied to the sensor it was not measured on."""

    def setUp(self):
        from test_presentation_monitor import ready_device

        self.device = ready_device()  # channels 4, 5 and 6
        self.extremes = AxisExtremes()
        for packet in StreamDecoder().feed(rotation_stream()):
            self.extremes.add(packet)

    def write(self, channels):
        document = build_document(self.extremes, 20.0, {"source": "live"}, channels)
        folder = tempfile.mkdtemp()
        path = Path(folder) / "mag.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        return path

    def test_matching_channels_are_accepted(self):
        path = self.write({0: "4", 1: "5", 2: "6"})
        calibrations = load_magnetometer_calibrations(path, self.device)
        self.assertEqual(len(calibrations), 3)

    def test_the_old_channel_map_is_refused(self):
        # The assignment before the hardware change was 0, 1 and 4.
        path = self.write({0: "0", 1: "1", 2: "4"})
        with self.assertRaises(ValueError) as caught:
            load_magnetometer_calibrations(path, self.device)
        self.assertIn("channel 0", str(caught.exception))

    def test_a_file_without_channels_is_still_accepted(self):
        path = self.write(None)
        calibrations = load_magnetometer_calibrations(path, self.device)
        self.assertEqual(len(calibrations), 3)

    def test_no_device_means_no_check(self):
        path = self.write({0: "0", 1: "1", 2: "4"})
        self.assertEqual(len(load_magnetometer_calibrations(path, None)), 3)


if __name__ == "__main__":
    unittest.main()
