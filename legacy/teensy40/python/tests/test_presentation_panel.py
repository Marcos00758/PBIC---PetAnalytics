import json
import math
import sys

import numpy as np
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from parse_data import MagnetometerCalibration
from presentation_panel import (
    AxisScaler,
    BARO_COLUMNS,
    COLUMN_ABSOLUTE_PRESSURE,
    COLUMN_ALTITUDE,
    COLUMN_PRESSURE,
    COLUMN_TEMPERATURE,
    IMU_COLUMNS,
    PacketConverter,
    RestCalibration,
    SampleBuffer,
    calibrate_at_rest,
    changed_mask,
    compensated_pressure_hpa,
    load_magnetometer_calibrations,
    relative_altitude_cm,
    trailing_average,
)
from test_presentation_monitor import (
    BMP_SENTINEL,
    StreamDecoder,
    healthy_stream,
    make_nvm,
    make_packet,
    ready_device,
)


def decode(stream: bytes):
    return StreamDecoder().feed(stream)


class RelativeAltitudeTest(unittest.TestCase):
    def test_same_pressure_means_no_height_change(self):
        self.assertAlmostEqual(relative_altitude_cm(1013.25, 1013.25), 0.0, places=6)

    def test_lower_pressure_reads_as_higher(self):
        self.assertGreater(relative_altitude_cm(1012.0, 1013.25), 0.0)

    def test_higher_pressure_reads_as_lower(self):
        self.assertLess(relative_altitude_cm(1014.0, 1013.25), 0.0)

    def test_one_hectopascal_is_roughly_eight_metres(self):
        centimetres = relative_altitude_cm(1012.25, 1013.25)
        self.assertAlmostEqual(centimetres / 100.0, 8.4, delta=0.6)

    def test_missing_reference_is_not_a_number(self):
        self.assertTrue(math.isnan(relative_altitude_cm(1013.0, None)))

    def test_invalid_pressure_is_not_a_number(self):
        self.assertTrue(math.isnan(relative_altitude_cm(0.0, 1013.0)))


class CalibrateAtRestTest(unittest.TestCase):
    def setUp(self):
        self.device = ready_device()
        self.bmp = tuple(self.device.calibration(index) for index in range(2))

    def test_measures_gravity_and_zero_gyro_bias(self):
        packets = decode(healthy_stream())
        calibration = calibrate_at_rest(packets, self.device, self.bmp)

        self.assertTrue(calibration.valid)
        self.assertEqual(calibration.samples, len(packets))
        for sensor in range(3):
            self.assertAlmostEqual(
                calibration.gravity_norm_mps2[sensor], 9.807, places=2
            )
            for axis in range(3):
                self.assertAlmostEqual(
                    calibration.gyro_bias_dps[sensor][axis], 0.0, places=6
                )

    def test_captures_gyro_bias_when_the_sensor_reads_offset(self):
        from test_presentation_monitor import healthy_bmp, healthy_values

        stream = b""
        for index in range(100):
            values = list(healthy_values(index))
            values[3] = 100  # steady offset on icm0 gyro x
            stream += make_packet(
                index * 10_000, index, tuple(values), healthy_bmp(index)
            )
        calibration = calibrate_at_rest(decode(stream), self.device, self.bmp)

        expected = 100 * self.device.gyro_scale_dps
        self.assertAlmostEqual(calibration.gyro_bias_dps[0][0], expected, places=4)
        self.assertAlmostEqual(calibration.gyro_bias_dps[1][0], 0.0, places=6)

    def test_records_a_pressure_datum_per_barometer(self):
        calibration = calibrate_at_rest(decode(healthy_stream()), self.device, self.bmp)
        for index in range(2):
            self.assertIsNotNone(calibration.reference_pressure_hpa[index])
            self.assertAlmostEqual(
                calibration.reference_pressure_hpa[index], 1013.20, places=1
            )

    def test_sentinel_only_capture_has_no_pressure_datum(self):
        from test_presentation_monitor import healthy_values

        stream = b"".join(
            make_packet(
                index * 10_000,
                index,
                healthy_values(index),
                (BMP_SENTINEL, BMP_SENTINEL, BMP_SENTINEL, BMP_SENTINEL),
            )
            for index in range(50)
        )
        calibration = calibrate_at_rest(decode(stream), self.device, self.bmp)
        self.assertIsNone(calibration.reference_pressure_hpa[0])

    def test_empty_capture_is_not_valid(self):
        calibration = calibrate_at_rest([], self.device, self.bmp)
        self.assertFalse(calibration.valid)


class PacketConverterTest(unittest.TestCase):
    def setUp(self):
        self.device = ready_device()
        self.bmp = tuple(self.device.calibration(index) for index in range(2))
        self.mag = (MagnetometerCalibration(),) * 3
        self.packets = decode(healthy_stream())
        self.calibration = calibrate_at_rest(self.packets, self.device, self.bmp)

    def test_removing_gravity_centres_a_still_sensor(self):
        converter = PacketConverter(
            self.device, self.mag, self.bmp, self.calibration, remove_gravity=True
        )
        row = converter.imu_row(self.packets[10])
        self.assertEqual(len(row), IMU_COLUMNS)
        for axis in range(3):
            self.assertAlmostEqual(row[axis], 0.0, delta=0.02)

    def test_keeping_gravity_shows_the_full_vector(self):
        converter = PacketConverter(
            self.device, self.mag, self.bmp, self.calibration, remove_gravity=False
        )
        row = converter.imu_row(self.packets[10])
        norm = math.sqrt(sum(value * value for value in row[0:3]))
        self.assertAlmostEqual(norm, 9.807, places=2)

    def test_gyro_bias_is_subtracted(self):
        from test_presentation_monitor import healthy_bmp, healthy_values

        values = list(healthy_values(0))
        values[3] = 100
        stream = b"".join(
            make_packet(index * 10_000, index, tuple(values), healthy_bmp(index))
            for index in range(100)
        )
        packets = decode(stream)
        calibration = calibrate_at_rest(packets, self.device, self.bmp)
        converter = PacketConverter(self.device, self.mag, self.bmp, calibration)
        row = converter.imu_row(packets[5])
        self.assertAlmostEqual(row[3], 0.0, places=5)

    def test_magnetometer_uses_the_supplied_calibration(self):
        shifted = (MagnetometerCalibration(hard_iron_ut=(10.0, 0.0, 0.0)),) * 3
        plain = PacketConverter(self.device, self.mag, self.bmp, self.calibration)
        offset = PacketConverter(self.device, shifted, self.bmp, self.calibration)
        difference = (
            plain.imu_row(self.packets[0])[6] - offset.imu_row(self.packets[0])[6]
        )
        self.assertAlmostEqual(difference, 10.0, places=5)

    def test_baro_row_reports_deviation_altitude_temperature_and_absolute(self):
        converter = PacketConverter(self.device, self.mag, self.bmp, self.calibration)
        row = converter.baro_row(self.packets[0])
        self.assertEqual(len(row), BARO_COLUMNS)
        # The still capture sits on its own reference, so the deviation is ~0 Pa
        # while the absolute column still carries the real pressure.
        self.assertAlmostEqual(row[COLUMN_PRESSURE], 0.0, delta=1.0)
        self.assertAlmostEqual(row[COLUMN_ALTITUDE], 0.0, delta=5.0)
        self.assertAlmostEqual(row[COLUMN_TEMPERATURE], 0.0, places=2)
        self.assertAlmostEqual(row[COLUMN_ABSOLUTE_PRESSURE], 1013.20, places=1)

    def test_deviation_is_reported_in_pascal(self):
        # The measurement stays at 1013.20 hPa while the reference sits one
        # hectopascal lower, so the deviation must read as one hundred pascal.
        shifted = RestCalibration(
            samples=self.calibration.samples,
            gyro_bias_dps=self.calibration.gyro_bias_dps,
            gravity_mps2=self.calibration.gravity_mps2,
            gravity_norm_mps2=self.calibration.gravity_norm_mps2,
            reference_pressure_hpa=(1012.20, 1012.20),
        )
        converter = PacketConverter(self.device, self.mag, self.bmp, shifted)
        row = converter.baro_row(self.packets[0])

        self.assertAlmostEqual(row[COLUMN_PRESSURE], 100.0, delta=1.0)
        self.assertAlmostEqual(row[COLUMN_ABSOLUTE_PRESSURE], 1013.20, places=1)

    def test_higher_pressure_than_the_reference_reads_as_lower_altitude(self):
        shifted = RestCalibration(
            samples=self.calibration.samples,
            gyro_bias_dps=self.calibration.gyro_bias_dps,
            gravity_mps2=self.calibration.gravity_mps2,
            gravity_norm_mps2=self.calibration.gravity_norm_mps2,
            reference_pressure_hpa=(1012.20, 1012.20),
        )
        converter = PacketConverter(self.device, self.mag, self.bmp, shifted)
        row = converter.baro_row(self.packets[0])
        # One hectopascal above the datum is roughly eight metres below it.
        self.assertAlmostEqual(row[COLUMN_ALTITUDE] / 100.0, -8.4, delta=0.6)

    def test_baro_row_has_no_deviation_without_a_reference(self):
        converter = PacketConverter(self.device, self.mag, self.bmp, None)
        row = converter.baro_row(self.packets[0])
        self.assertTrue(math.isnan(row[COLUMN_PRESSURE]))
        self.assertTrue(math.isnan(row[COLUMN_ALTITUDE]))
        self.assertFalse(math.isnan(row[COLUMN_ABSOLUTE_PRESSURE]))

    def test_baro_row_is_not_a_number_without_calibration(self):
        converter = PacketConverter(
            self.device, self.mag, (None, None), self.calibration
        )
        row = converter.baro_row(self.packets[0])
        self.assertTrue(math.isnan(row[COLUMN_PRESSURE]))
        self.assertTrue(math.isnan(row[COLUMN_ALTITUDE]))


class AxisScalerTest(unittest.TestCase):
    def test_first_update_adopts_the_data(self):
        low, high = AxisScaler(minimum_span=2.0).update(-5.0, 5.0)
        self.assertLess(low, -5.0)
        self.assertGreater(high, 5.0)

    def test_never_zooms_tighter_than_the_minimum_span(self):
        low, high = AxisScaler(minimum_span=10.0).update(-0.01, 0.01)
        self.assertGreaterEqual(high - low, 10.0)

    def test_growth_is_immediate(self):
        scaler = AxisScaler(minimum_span=1.0)
        scaler.update(-1.0, 1.0)
        low, high = scaler.update(-100.0, 100.0)
        # A spike must never be clipped, so the wider range applies at once.
        self.assertLessEqual(low, -100.0)
        self.assertGreaterEqual(high, 100.0)

    def test_shrinking_is_gradual(self):
        scaler = AxisScaler(minimum_span=1.0, shrink_rate=0.1)
        scaler.update(-100.0, 100.0)
        low, high = scaler.update(-1.0, 1.0)
        # Still wide right after the spike: this is what stops the breathing.
        self.assertLess(low, -50.0)
        self.assertGreater(high, 50.0)

    def test_shrinking_eventually_reaches_the_target(self):
        scaler = AxisScaler(minimum_span=1.0, shrink_rate=0.2)
        scaler.update(-100.0, 100.0)
        for _ in range(300):
            low, high = scaler.update(-1.0, 1.0)
        self.assertAlmostEqual(high - low, 2.0 * 1.1, delta=0.1)


class AdaptiveGravityTest(unittest.TestCase):
    def setUp(self):
        self.device = ready_device()
        self.bmp = tuple(self.device.calibration(index) for index in range(2))
        self.mag = (MagnetometerCalibration(),) * 3
        self.packets = decode(healthy_stream())
        self.calibration = calibrate_at_rest(self.packets, self.device, self.bmp)

    def test_a_reoriented_sensor_returns_to_zero(self):
        from test_presentation_monitor import healthy_bmp, healthy_values

        converter = PacketConverter(
            self.device, self.mag, self.bmp, self.calibration, remove_gravity=True
        )
        converter.reset_gravity_tracking()

        # Gravity now lies on x instead of z, as if the sensor were laid on its
        # side after calibration. A frozen vector would leave this offset for
        # the rest of the session.
        values = list(healthy_values(0))
        values[0], values[2] = values[2], values[0]
        rotated = decode(
            b"".join(
                make_packet(i * 10_000, i, tuple(values), healthy_bmp(i))
                for i in range(1500)
            )
        )

        first = converter.imu_row(rotated[0])
        self.assertGreater(abs(first[0]), 5.0)
        for packet in rotated:
            last = converter.imu_row(packet)
        for axis in range(3):
            self.assertAlmostEqual(last[axis], 0.0, delta=0.2)

    def test_the_estimate_follows_even_while_gravity_is_shown(self):
        converter = PacketConverter(
            self.device, self.mag, self.bmp, self.calibration, remove_gravity=False
        )
        converter.reset_gravity_tracking()
        for packet in self.packets:
            row = converter.imu_row(packet)
        # Showing gravity must report the real vector, not a centred one.
        self.assertAlmostEqual(
            math.sqrt(sum(value * value for value in row[0:3])), 9.807, places=2
        )
        # Toggling now must not produce a step, so the tracker had to keep up.
        converter.remove_gravity = True
        centred = converter.imu_row(self.packets[-1])
        for axis in range(3):
            self.assertAlmostEqual(centred[axis], 0.0, delta=0.05)


class PressureReferenceTest(unittest.TestCase):
    def setUp(self):
        self.device = ready_device()
        self.bmp = tuple(self.device.calibration(index) for index in range(2))

    def test_steady_capture_is_reported_as_settled(self):
        calibration = calibrate_at_rest(
            decode(healthy_stream()), self.device, self.bmp
        )
        self.assertTrue(calibration.pressure_settled)
        self.assertTrue(calibration.barometers_agree)

    def test_disagreeing_datums_are_detected(self):
        calibration = RestCalibration(
            samples=10,
            gyro_bias_dps=((0.0, 0.0, 0.0),) * 3,
            gravity_mps2=((0.0, 0.0, 9.8),) * 3,
            gravity_norm_mps2=(9.8,) * 3,
            reference_pressure_hpa=(1009.70, 1007.50),
        )
        # The two sit on one board; two hectopascal apart is eighteen metres.
        self.assertFalse(calibration.barometers_agree)

    def test_drift_during_calibration_is_detected(self):
        calibration = RestCalibration(
            samples=10,
            gyro_bias_dps=((0.0, 0.0, 0.0),) * 3,
            gravity_mps2=((0.0, 0.0, 9.8),) * 3,
            gravity_norm_mps2=(9.8,) * 3,
            reference_pressure_hpa=(1009.70, 1009.70),
            pressure_drift_hpa=(0.0, 1.5),
        )
        self.assertFalse(calibration.pressure_settled)


class TrailingAverageTest(unittest.TestCase):
    def test_window_of_one_changes_nothing(self):
        values = np.array([1.0, 5.0, 3.0])
        self.assertTrue(np.array_equal(trailing_average(values, 1), values))

    def test_expands_over_the_first_samples(self):
        values = np.array([1.0, 3.0, 5.0, 7.0])
        result = trailing_average(values, 2)
        self.assertAlmostEqual(result[0], 1.0)
        self.assertAlmostEqual(result[1], 2.0)
        self.assertAlmostEqual(result[3], 6.0)

    def test_never_looks_into_the_future(self):
        values = np.array([0.0, 0.0, 0.0, 100.0])
        result = trailing_average(values, 4)
        self.assertAlmostEqual(result[2], 0.0)
        self.assertAlmostEqual(result[3], 25.0)

    def test_reduces_the_spread_of_noise(self):
        generator = np.random.default_rng(7)
        noise = generator.normal(0.0, 1.0, 4000)
        filtered = trailing_average(noise, 25)
        self.assertLess(filtered.std(), noise.std() / 3.0)

    def test_empty_input_is_returned_unchanged(self):
        self.assertEqual(trailing_average(np.empty(0), 10).size, 0)


class ChangedMaskTest(unittest.TestCase):
    def test_empty_input(self):
        self.assertEqual(changed_mask(np.empty(0)).size, 0)

    def test_first_and_last_samples_always_count(self):
        # A steady sensor must still reach the right edge of the plot.
        mask = changed_mask(np.array([4.0, 4.0, 4.0, 4.0]))
        self.assertTrue(mask[0])
        self.assertTrue(mask[-1])
        self.assertFalse(mask[1:-1].any())

    def test_single_sample_is_kept(self):
        self.assertTrue(changed_mask(np.array([1.0])).all())

    def test_marks_only_new_readings(self):
        # Two barometer readings, each repeated from cache into four packets.
        # The two transitions are kept, plus the newest sample as the edge.
        values = np.array([10.0, 10.0, 10.0, 10.0, 11.0, 11.0, 11.0, 11.0])
        mask = changed_mask(values)
        self.assertEqual(mask.sum(), 3)
        self.assertTrue(mask[0])
        self.assertTrue(mask[4])
        self.assertTrue(mask[-1])
        self.assertFalse(mask[1:4].any())

    def test_keeps_everything_when_every_sample_differs(self):
        values = np.arange(50, dtype=float)
        self.assertTrue(changed_mask(values).all())


class CompensatedPressureTest(unittest.TestCase):
    def test_sentinel_is_not_a_number(self):
        from test_presentation_monitor import healthy_values

        packet = decode(
            make_packet(
                0,
                0,
                healthy_values(0),
                (BMP_SENTINEL, BMP_SENTINEL, BMP_SENTINEL, BMP_SENTINEL),
            )
        )[0]
        device = ready_device()
        pressure, celsius = compensated_pressure_hpa(packet, 0, device.calibration(0))
        self.assertTrue(math.isnan(pressure))
        self.assertTrue(math.isnan(celsius))


class SampleBufferTest(unittest.TestCase):
    def make_buffer(self, capacity=10):
        return SampleBuffer(capacity)

    def test_rejects_a_useless_capacity(self):
        with self.assertRaises(ValueError):
            SampleBuffer(1)

    def test_window_is_empty_before_any_sample(self):
        times, imu, baro = self.make_buffer().window(5.0)
        self.assertEqual(times.size, 0)
        self.assertEqual(imu.shape, (0, IMU_COLUMNS))
        self.assertEqual(baro.shape, (0, BARO_COLUMNS))

    def test_time_is_relative_to_the_newest_sample(self):
        buffer = self.make_buffer()
        for index in range(5):
            buffer.append(index * 0.01, [float(index)] * IMU_COLUMNS,
                          [0.0] * BARO_COLUMNS)
        times, imu, _ = buffer.window(1.0)
        self.assertEqual(len(times), 5)
        self.assertAlmostEqual(times[-1], 0.0)
        self.assertAlmostEqual(times[0], -0.04)
        self.assertAlmostEqual(imu[-1][0], 4.0)

    def test_window_keeps_only_the_requested_seconds(self):
        buffer = self.make_buffer(capacity=200)
        for index in range(100):
            buffer.append(index * 0.01, [0.0] * IMU_COLUMNS, [0.0] * BARO_COLUMNS)
        times, _, _ = buffer.window(0.2)
        self.assertLessEqual(len(times), 22)
        self.assertGreaterEqual(times[0], -0.2)

    def test_halves_itself_instead_of_overflowing(self):
        buffer = self.make_buffer(capacity=10)
        for index in range(25):
            buffer.append(index * 0.01, [float(index)] * IMU_COLUMNS,
                          [0.0] * BARO_COLUMNS)
        self.assertLessEqual(len(buffer), 10)
        _, imu, _ = buffer.window(10.0)
        self.assertAlmostEqual(imu[-1][0], 24.0)


class LoadMagnetometerCalibrationsTest(unittest.TestCase):
    def test_missing_path_returns_identity(self):
        calibrations = load_magnetometer_calibrations(None)
        self.assertEqual(len(calibrations), 3)
        self.assertEqual(calibrations[0].hard_iron_ut, (0.0, 0.0, 0.0))

    def test_reads_the_calibrate_magnetometer_document(self):
        document = {
            f"icm{sensor}": {
                "hard_iron_ut": [1.0 + sensor, 2.0, 3.0],
                "soft_iron_matrix": [[1.1, 0, 0], [0, 1.2, 0], [0, 0, 1.3]],
            }
            for sensor in range(3)
        }
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "mag.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            calibrations = load_magnetometer_calibrations(path)
        self.assertAlmostEqual(calibrations[2].hard_iron_ut[0], 3.0)
        self.assertAlmostEqual(calibrations[0].soft_iron_matrix[1][1], 1.2)

    def test_rejects_a_malformed_document(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "mag.json"
            path.write_text(json.dumps({"icm0": {"hard_iron_ut": [1.0]}}), "utf-8")
            with self.assertRaises(ValueError):
                load_magnetometer_calibrations(path)


class LiveVerdictTest(unittest.TestCase):
    def test_motion_does_not_fail_the_live_strip(self):
        from presentation_monitor import evaluate_icm
        from test_presentation_monitor import healthy_bmp, healthy_values

        values = list(healthy_values(0))
        values[3] = 20_000  # a fast rotation, normal while presenting
        stream = b"".join(
            make_packet(index * 10_000, index, tuple(values), healthy_bmp(index))
            for index in range(100)
        )
        packets = decode(stream)
        device = ready_device()

        still = evaluate_icm(0, packets, device, 0.99, at_rest=True)
        moving = evaluate_icm(0, packets, device, 0.99, at_rest=False)

        self.assertIn("gyro_not_at_rest", still.reasons)
        self.assertNotIn("gyro_not_at_rest", moving.reasons)


if __name__ == "__main__":
    unittest.main()
