"""Live pyqtgraph panel for the three ICM-20948 and the two BMP390."""

from __future__ import annotations

import argparse
import math
import statistics
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from capture_serial import READ_TIMEOUT_SECONDS, SERIAL_BAUD, detect_port
from parse_data import (
    BMP_RAW_INVALID,
    Bmp390Calibration,
    ImuPacket,
    MagnetometerCalibration,
    elapsed_us,
)
from presentation_monitor import (
    BANNER_TIMEOUT_SECONDS,
    DeviceInfo,
    FAIL,
    GRAVITY_MPS2,
    OK,
    SerialSource,
    StreamDecoder,
    WARN,
    evaluate_all,
    wait_for_banner,
)

try:
    import serial
except ImportError as exc:  # pragma: no cover - mirrors capture_serial.py
    raise SystemExit(
        "pyserial is required; install it with: pip install -r python/requirements.txt"
    ) from exc

SENSOR_COUNT = 3
BMP_COUNT = 2
AXIS_NAMES = ("x", "y", "z")
IMU_COLUMNS = 27
BARO_COLUMNS = 8

# Every filter is a switch. Nothing smooths the data unless one of these is on,
# and the footer always states which are active.
PRESSURE_FILTER_ENABLED = True
IMU_FILTER_ENABLED = True
DECIMATION_ENABLED = True

DEFAULT_PRESSURE_FILTER_SECONDS = 1.0
DEFAULT_IMU_FILTER_SECONDS = 0.05
PASCAL_PER_HECTOPASCAL = 100.0

# The BMP390 IIR filter converges over roughly a second after configuration.
# A reference captured mid-ramp is wrong for the whole session, so the datum is
# a median and the drift across the window is reported.
PRESSURE_DRIFT_LIMIT_HPA = 0.20
PRESSURE_AGREEMENT_LIMIT_HPA = 0.50

# Gravity is tracked instead of frozen. A vector captured once only holds while
# the orientation never changes, and a presentation means picking sensors up.
GRAVITY_TRACKING_SECONDS = 3.0

# The axis grows at once and shrinks slowly, so one spike cannot make it breathe.
SCALE_SHRINK_RATE = 0.03
SCALE_PADDING = 1.1

# Smallest span each plot may zoom into. Without a floor the autorange fills the
# panel with the noise of a sensor that is simply sitting still.
MINIMUM_SPAN_ACCEL_MPS2 = 2.0
MINIMUM_SPAN_GYRO_DPS = 40.0
MINIMUM_SPAN_MAG_UT = 20.0
MINIMUM_SPAN_PRESSURE_PA = 20.0
MINIMUM_SPAN_ALTITUDE_CM = 100.0
MINIMUM_SPANS = (
    MINIMUM_SPAN_ACCEL_MPS2,
    MINIMUM_SPAN_GYRO_DPS,
    MINIMUM_SPAN_MAG_UT,
)

DEFAULT_WINDOW_SECONDS = 10.0
MINIMUM_WINDOW_SECONDS = 2.0
MAXIMUM_WINDOW_SECONDS = 60.0
WINDOW_STEP_SECONDS = 2.0
DEFAULT_CALIBRATION_SECONDS = 3.0
REDRAW_INTERVAL_MS = 33
VERDICT_INTERVAL_MS = 500
VERDICT_WINDOW_SECONDS = 2.0

# International Standard Atmosphere, the same relation used by the Adafruit
# BMP3XX helper. Referenced to the pressure captured during calibration, so the
# result is a height difference and never an absolute altitude.
ALTITUDE_SCALE_M = 44330.0
ALTITUDE_EXPONENT = 1.0 / 5.255
CENTIMETRES_PER_METRE = 100.0

AXIS_COLORS = ("#1f77b4", "#d95f02", "#2ca02c")
LEVEL_COLORS = {OK: "#1a7f37", WARN: "#b26a00", FAIL: "#c1121f"}
QUANTITY_TITLES = (
    "Acelerometro (m/s2)",
    "Giroscopio (graus/s)",
    "Magnetometro (uT)",
)

COLUMN_PRESSURE = 0
COLUMN_ALTITUDE = COLUMN_PRESSURE + BMP_COUNT
COLUMN_TEMPERATURE = COLUMN_ALTITUDE + BMP_COUNT
COLUMN_ABSOLUTE_PRESSURE = COLUMN_TEMPERATURE + BMP_COUNT


def trailing_average(values: np.ndarray, window: int) -> np.ndarray:
    """Causal mean over the last `window` samples, expanding at the start."""
    if window <= 1 or values.size == 0:
        return values.astype(np.float64)
    cumulative = np.concatenate(([0.0], np.cumsum(values, dtype=np.float64)))
    indices = np.arange(1, values.size + 1)
    starts = np.maximum(indices - window, 0)
    return (cumulative[indices] - cumulative[starts]) / (indices - starts)


def changed_mask(values: np.ndarray) -> np.ndarray:
    """Mark the samples that carry a new reading rather than a repeated one.

    The newest sample is always kept. A perfectly steady barometer would
    otherwise collapse to a single old point and the curve would stop short of
    the right edge, reading as a dead sensor instead of a quiet one.
    """
    if values.size == 0:
        return np.zeros(0, dtype=bool)
    mask = np.empty(values.size, dtype=bool)
    mask[0] = True
    np.not_equal(values[1:], values[:-1], out=mask[1:])
    mask[-1] = True
    return mask


@dataclass(frozen=True)
class RestCalibration:
    """Bias and reference values measured with the device held still."""

    samples: int
    gyro_bias_dps: tuple[tuple[float, float, float], ...]
    gravity_mps2: tuple[tuple[float, float, float], ...]
    gravity_norm_mps2: tuple[float, ...]
    reference_pressure_hpa: tuple[float | None, ...]
    pressure_drift_hpa: tuple[float, ...] = (0.0, 0.0)

    @property
    def valid(self) -> bool:
        return self.samples > 0

    @property
    def pressure_settled(self) -> bool:
        """False when a barometer was still converging during calibration."""
        return all(
            drift <= PRESSURE_DRIFT_LIMIT_HPA for drift in self.pressure_drift_hpa
        )

    @property
    def barometers_agree(self) -> bool:
        """The two sit on one board, so their datums must be close."""
        usable = [value for value in self.reference_pressure_hpa if value is not None]
        if len(usable) < 2:
            return True
        return max(usable) - min(usable) <= PRESSURE_AGREEMENT_LIMIT_HPA


def relative_altitude_cm(pressure_hpa: float, reference_hpa: float | None) -> float:
    """Height above the calibration point, positive when the device rises."""
    if reference_hpa is None or reference_hpa <= 0 or pressure_hpa <= 0:
        return math.nan
    ratio = pressure_hpa / reference_hpa
    metres = ALTITUDE_SCALE_M * (1.0 - ratio**ALTITUDE_EXPONENT)
    return metres * CENTIMETRES_PER_METRE


def compensated_pressure_hpa(
    packet: ImuPacket, index: int, calibration: Bmp390Calibration | None
) -> tuple[float, float]:
    """Return pressure in hectopascal and temperature in Celsius, or NaN."""
    pressure_raw, temperature_raw = packet.bmp_raw[index]
    if (
        calibration is None
        or pressure_raw == BMP_RAW_INVALID
        or temperature_raw == BMP_RAW_INVALID
    ):
        return math.nan, math.nan
    pressure_pa, celsius = calibration.compensate(pressure_raw, temperature_raw)
    return pressure_pa / 100.0, celsius


def calibrate_at_rest(
    packets: list[ImuPacket],
    device: DeviceInfo,
    bmp_calibrations: tuple[Bmp390Calibration | None, ...],
) -> RestCalibration:
    """Average a still capture into gyro bias, gravity and a pressure datum."""
    if not packets:
        return RestCalibration(0, (), (), (), (None,) * BMP_COUNT)

    accel_scale = device.accel_scale_mps2
    gyro_scale = device.gyro_scale_dps
    gyro_bias: list[tuple[float, float, float]] = []
    gravity: list[tuple[float, float, float]] = []
    gravity_norm: list[float] = []

    for sensor in range(SENSOR_COUNT):
        base = sensor * 9
        accel_sums = [0.0, 0.0, 0.0]
        gyro_sums = [0.0, 0.0, 0.0]
        for packet in packets:
            for axis in range(3):
                accel_sums[axis] += packet.values[base + axis] * accel_scale
                gyro_sums[axis] += packet.values[base + 3 + axis] * gyro_scale
        count = float(len(packets))
        accel_mean = tuple(value / count for value in accel_sums)
        gyro_bias.append(tuple(value / count for value in gyro_sums))
        gravity.append(accel_mean)
        gravity_norm.append(math.sqrt(sum(value * value for value in accel_mean)))

    reference: list[float | None] = []
    drift: list[float] = []
    for index in range(BMP_COUNT):
        readings = [
            compensated_pressure_hpa(packet, index, bmp_calibrations[index])[0]
            for packet in packets
        ]
        usable = [value for value in readings if not math.isnan(value)]
        if not usable:
            reference.append(None)
            drift.append(0.0)
            continue
        # The median ignores a ramp at the start of the window; the mean would
        # be dragged down by it and poison the datum for the whole session.
        reference.append(statistics.median(usable))
        middle = len(usable) // 2
        if middle:
            drift.append(
                abs(
                    statistics.fmean(usable[middle:])
                    - statistics.fmean(usable[:middle])
                )
            )
        else:
            drift.append(0.0)

    return RestCalibration(
        samples=len(packets),
        gyro_bias_dps=tuple(gyro_bias),
        gravity_mps2=tuple(gravity),
        gravity_norm_mps2=tuple(gravity_norm),
        reference_pressure_hpa=tuple(reference),
        pressure_drift_hpa=tuple(drift),
    )


class AxisScaler:
    """Vertical range that grows at once and shrinks slowly."""

    def __init__(
        self, minimum_span: float, shrink_rate: float = SCALE_SHRINK_RATE
    ) -> None:
        self.minimum_span = minimum_span
        self.shrink_rate = shrink_rate
        self.low: float | None = None
        self.high: float | None = None

    def update(self, data_low: float, data_high: float) -> tuple[float, float]:
        centre = (data_low + data_high) / 2.0
        span = max(data_high - data_low, self.minimum_span) * SCALE_PADDING
        target_low = centre - span / 2.0
        target_high = centre + span / 2.0

        if self.low is None or self.high is None:
            self.low, self.high = target_low, target_high
            return self.low, self.high

        # A wider target takes effect immediately so nothing is ever clipped.
        # A narrower one is approached gradually, which is what stops a single
        # spike from making the whole axis breathe in and out.
        self.low = (
            target_low
            if target_low < self.low
            else self.low + (target_low - self.low) * self.shrink_rate
        )
        self.high = (
            target_high
            if target_high > self.high
            else self.high + (target_high - self.high) * self.shrink_rate
        )
        return self.low, self.high


class PacketConverter:
    """Turns raw packets into the physical rows the plots consume."""

    def __init__(
        self,
        device: DeviceInfo,
        mag_calibrations: tuple[MagnetometerCalibration, ...],
        bmp_calibrations: tuple[Bmp390Calibration | None, ...],
        calibration: RestCalibration | None = None,
        remove_gravity: bool = True,
    ) -> None:
        self.device = device
        self.mag_calibrations = mag_calibrations
        self.bmp_calibrations = bmp_calibrations
        self.calibration = calibration
        self.remove_gravity = remove_gravity
        self._gravity: list[list[float]] | None = None
        self._gravity_alpha = 1.0 - math.exp(
            -1.0 / max(1.0, device.imu_rate_hz * GRAVITY_TRACKING_SECONDS)
        )

    def reset_gravity_tracking(self) -> None:
        """Seed the tracker from the calibration so it starts converged."""
        if self.calibration is not None and self.calibration.valid:
            self._gravity = [list(vector) for vector in self.calibration.gravity_mps2]
        else:
            self._gravity = None

    def imu_row(self, packet: ImuPacket) -> list[float]:
        accel_scale = self.device.accel_scale_mps2
        gyro_scale = self.device.gyro_scale_dps
        row: list[float] = []
        for sensor in range(SENSOR_COUNT):
            base = sensor * 9
            for axis in range(3):
                value = packet.values[base + axis] * accel_scale
                # The estimate follows the sensor even while it is displayed
                # raw, so toggling the key never produces a step.
                if self._gravity is None:
                    self._gravity = [
                        [0.0, 0.0, 0.0] for _ in range(SENSOR_COUNT)
                    ]
                    if self.calibration is not None and self.calibration.valid:
                        self._gravity = [
                            list(vector) for vector in self.calibration.gravity_mps2
                        ]
                    else:
                        self._gravity[sensor][axis] = value
                estimate = self._gravity[sensor][axis]
                estimate += self._gravity_alpha * (value - estimate)
                self._gravity[sensor][axis] = estimate
                row.append(value - estimate if self.remove_gravity else value)
            for axis in range(3):
                value = packet.values[base + 3 + axis] * gyro_scale
                if self.calibration is not None:
                    value -= self.calibration.gyro_bias_dps[sensor][axis]
                row.append(value)
            magnetic = self.mag_calibrations[sensor].apply(
                packet.values[base + 6 : base + 9]
            )
            row.extend(magnetic)
        return row

    def baro_row(self, packet: ImuPacket) -> list[float]:
        """Pressure as a deviation in pascal, altitude, temperature, absolute.

        The two barometers sit about ten pascal apart by calibration alone.
        Plotting the deviation from each one's own reference overlays them and
        spends the axis on the change instead of on that constant offset.
        """
        deviations: list[float] = []
        altitudes: list[float] = []
        temperatures: list[float] = []
        absolute: list[float] = []
        for index in range(BMP_COUNT):
            pressure, celsius = compensated_pressure_hpa(
                packet, index, self.bmp_calibrations[index]
            )
            reference = (
                self.calibration.reference_pressure_hpa[index]
                if self.calibration is not None and self.calibration.valid
                else None
            )
            absolute.append(pressure)
            temperatures.append(celsius)
            if math.isnan(pressure) or reference is None:
                deviations.append(math.nan)
                altitudes.append(math.nan)
                continue
            deviations.append((pressure - reference) * PASCAL_PER_HECTOPASCAL)
            altitudes.append(relative_altitude_cm(pressure, reference))
        return deviations + altitudes + temperatures + absolute


class SampleBuffer:
    """Fixed-capacity history that halves itself instead of reallocating."""

    def __init__(self, capacity: int) -> None:
        if capacity < 2:
            raise ValueError("capacity must hold at least two samples")
        self.capacity = capacity
        self._time = np.zeros(capacity, dtype=np.float64)
        self._imu = np.zeros((capacity, IMU_COLUMNS), dtype=np.float32)
        self._baro = np.zeros((capacity, BARO_COLUMNS), dtype=np.float32)
        self._count = 0

    def __len__(self) -> int:
        return self._count

    def append(
        self, timestamp_s: float, imu_row: list[float], baro_row: list[float]
    ) -> None:
        if self._count == self.capacity:
            keep = self.capacity // 2
            start = self.capacity - keep
            self._time[:keep] = self._time[start:]
            self._imu[:keep] = self._imu[start:]
            self._baro[:keep] = self._baro[start:]
            self._count = keep
        index = self._count
        self._time[index] = timestamp_s
        self._imu[index] = imu_row
        self._baro[index] = baro_row
        self._count += 1

    def window(
        self, seconds: float
    ) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
        """Slice the most recent seconds, with time relative to the newest."""
        if self._count == 0:
            return (
                np.empty(0),
                np.empty((0, IMU_COLUMNS)),
                np.empty((0, BARO_COLUMNS)),
            )
        newest = self._time[self._count - 1]
        start = int(
            np.searchsorted(self._time[: self._count], newest - seconds, "left")
        )
        return (
            self._time[start : self._count] - newest,
            self._imu[start : self._count],
            self._baro[start : self._count],
        )


def load_magnetometer_calibrations(
    path: Path | None, device: DeviceInfo | None = None
) -> tuple[MagnetometerCalibration, ...]:
    """Read the JSON produced by calibrate_magnetometer.py.

    When the file records the multiplexer channel it was measured on, that
    channel must still match the running firmware. The assignment has changed
    once already, and applying an old file would silently correct each sensor
    with another sensor's iron.
    """
    if path is None:
        return (MagnetometerCalibration(),) * SENSOR_COUNT

    import json

    document = json.loads(path.read_text(encoding="utf-8"))
    calibrations = []
    for sensor in range(SENSOR_COUNT):
        entry = document.get(f"icm{sensor}", {})
        recorded = entry.get("mux_channel")
        if device is not None and recorded is not None:
            current = device.icms.get(sensor, {}).get("channel")
            if current is not None and str(recorded) != str(current):
                raise ValueError(
                    f"magnetometer calibration for icm{sensor} was measured on "
                    f"channel {recorded} but the firmware reports channel "
                    f"{current}; record a new rotation with "
                    "python/calibrate_magnetometer.py --live"
                )
        hard_iron = tuple(
            float(value) for value in entry.get("hard_iron_ut", (0, 0, 0))
        )
        matrix = tuple(
            tuple(float(value) for value in row)
            for row in entry.get(
                "soft_iron_matrix", ((1, 0, 0), (0, 1, 0), (0, 0, 1))
            )
        )
        if len(hard_iron) != 3 or len(matrix) != 3 or any(len(r) != 3 for r in matrix):
            raise ValueError(f"invalid magnetometer calibration for icm{sensor}")
        calibrations.append(MagnetometerCalibration(hard_iron, matrix))
    return tuple(calibrations)


class PresentationPanel:
    """Owns the window, the serial reader and the redraw timers."""

    def __init__(self, pg, qtcore, qtgui, qtwidgets, source, decoder, options):
        self._pg = pg
        self._qtcore = qtcore
        self._qtgui = qtgui
        self._source = source
        self._decoder = decoder
        self._options = options

        self.window_seconds = options.window
        self.paused = False
        self.scale_locked = False
        self.remove_gravity = True
        self.filter_pressure = PRESSURE_FILTER_ENABLED
        self.filter_imu = IMU_FILTER_ENABLED
        self.decimate = DECIMATION_ENABLED
        self._latest_altitude = [math.nan] * BMP_COUNT
        self.calibrating = True
        self._calibration_packets: list[ImuPacket] = []
        self._calibration_elapsed_us = 0
        self._elapsed_s = 0.0
        self._previous_timestamp_us: int | None = None
        self._recent: list[ImuPacket] = []
        self._verdict_text = "calibrando"

        device = decoder.device
        self._converter = PacketConverter(
            device,
            load_magnetometer_calibrations(options.mag_calibration, device),
            tuple(device.calibration(index) for index in range(BMP_COUNT)),
            calibration=None,
            remove_gravity=self.remove_gravity,
        )
        capacity = int(MAXIMUM_WINDOW_SECONDS * device.imu_rate_hz * 2)
        self._buffer = SampleBuffer(capacity)

        self._build_window(qtwidgets)
        self._install_shortcuts()
        self._start_timers()

    # ---------------------------------------------------------------- layout

    def _build_window(self, qtwidgets) -> None:
        pg = self._pg
        pg.setConfigOptions(antialias=True)
        self.widget = pg.GraphicsLayoutWidget(
            show=True, title="PBIC / Pet Analytics - painel ao vivo"
        )
        self.widget.resize(1500, 950)

        self.status = self.widget.addLabel("", row=0, col=0, colspan=3)
        self._plots: list[list[object]] = []
        self._curves: list[list[list[object]]] = []
        self._scalers: dict[tuple[int, int], AxisScaler] = {}

        for sensor in range(SENSOR_COUNT):
            row_plots = []
            row_curves = []
            for quantity in range(3):
                plot = self.widget.addPlot(row=sensor + 1, col=quantity)
                plot.showGrid(x=True, y=True, alpha=0.25)
                plot.setLabel("bottom", "tempo (s)")
                if sensor == 0:
                    plot.setTitle(QUANTITY_TITLES[quantity])
                if quantity == 0:
                    plot.setLabel("left", f"ICM{sensor} (canal {self._channel(sensor)})")
                plot.setXRange(-self.window_seconds, 0, padding=0)
                plot.enableAutoRange("y", False)
                curves = [
                    plot.plot(
                        pen=pg.mkPen(AXIS_COLORS[axis], width=2),
                        name=AXIS_NAMES[axis],
                        connect="finite",
                    )
                    for axis in range(3)
                ]
                row_plots.append(plot)
                row_curves.append(curves)
                self._scalers[(sensor, quantity)] = AxisScaler(
                    MINIMUM_SPANS[quantity]
                )
            self._plots.append(row_plots)
            self._curves.append(row_curves)

        self.pressure_plot = self.widget.addPlot(row=4, col=0, colspan=2)
        self.pressure_plot.showGrid(x=True, y=True, alpha=0.25)
        self.pressure_plot.setTitle("Barometros: desvio da referencia (Pa)")
        self.pressure_plot.enableAutoRange("y", False)
        self._pressure_scaler = AxisScaler(MINIMUM_SPAN_PRESSURE_PA)
        self.pressure_plot.setLabel("bottom", "tempo (s)")
        self.pressure_curves = [
            self.pressure_plot.plot(
                pen=pg.mkPen(AXIS_COLORS[index], width=2), connect="finite"
            )
            for index in range(BMP_COUNT)
        ]

        self.altitude_plot = self.widget.addPlot(row=4, col=2)
        self.altitude_plot.showGrid(x=True, y=True, alpha=0.25)
        self.altitude_plot.setTitle("Altitude relativa (cm)")
        self.altitude_plot.enableAutoRange("y", False)
        self._altitude_scaler = AxisScaler(MINIMUM_SPAN_ALTITUDE_CM)
        self.altitude_plot.setLabel("bottom", "tempo (s)")
        self.altitude_curves = [
            self.altitude_plot.plot(
                pen=self._pg.mkPen(AXIS_COLORS[index], width=2), connect="finite"
            )
            for index in range(BMP_COUNT)
        ]

        self.altitude_readout = self.widget.addLabel("", row=5, col=0, colspan=3)
        self.footer = self.widget.addLabel("", row=6, col=0, colspan=3)
        self._all_plots = [
            plot for row in self._plots for plot in row
        ] + [self.pressure_plot, self.altitude_plot]

    def _channel(self, sensor: int) -> str:
        return self._decoder.device.icms.get(sensor, {}).get("channel", "?")

    def _install_shortcuts(self) -> None:
        bindings = (
            ("Space", self.toggle_pause),
            ("G", self.toggle_gravity),
            ("L", self.toggle_scale_lock),
            ("R", self.restart_calibration),
            ("F", self.toggle_imu_filter),
            ("B", self.toggle_pressure_filter),
            ("D", self.toggle_decimation),
            ("Z", self.rezero_altitude),
            ("Up", lambda: self.adjust_window(WINDOW_STEP_SECONDS)),
            ("Down", lambda: self.adjust_window(-WINDOW_STEP_SECONDS)),
            ("Q", self.widget.close),
        )
        self._shortcuts = []
        for sequence, handler in bindings:
            shortcut = self._qtgui.QShortcut(
                self._qtgui.QKeySequence(sequence), self.widget
            )
            shortcut.activated.connect(handler)
            self._shortcuts.append(shortcut)

    def _start_timers(self) -> None:
        self._redraw_timer = self._qtcore.QTimer()
        self._redraw_timer.timeout.connect(self._tick)
        self._redraw_timer.start(REDRAW_INTERVAL_MS)

        self._verdict_timer = self._qtcore.QTimer()
        self._verdict_timer.timeout.connect(self._refresh_verdicts)
        self._verdict_timer.start(VERDICT_INTERVAL_MS)

    # --------------------------------------------------------------- controls

    def toggle_pause(self) -> None:
        self.paused = not self.paused

    def toggle_gravity(self) -> None:
        self.remove_gravity = not self.remove_gravity
        self._converter.remove_gravity = self.remove_gravity

    def toggle_scale_lock(self) -> None:
        self.scale_locked = not self.scale_locked
        for plot in self._all_plots:
            plot.enableAutoRange("y", not self.scale_locked)

    def adjust_window(self, delta: float) -> None:
        self.window_seconds = min(
            MAXIMUM_WINDOW_SECONDS,
            max(MINIMUM_WINDOW_SECONDS, self.window_seconds + delta),
        )
        for plot in self._all_plots:
            plot.setXRange(-self.window_seconds, 0, padding=0)

    def toggle_imu_filter(self) -> None:
        self.filter_imu = not self.filter_imu

    def toggle_pressure_filter(self) -> None:
        self.filter_pressure = not self.filter_pressure

    def toggle_decimation(self) -> None:
        self.decimate = not self.decimate

    def rezero_altitude(self) -> None:
        """Adopt the current reading as the new altitude datum.

        Faster than a full recalibration and the usual fix when a barometer was
        still settling at startup: it needs no stillness, only the device where
        you want zero to be.
        """
        calibration = self._converter.calibration
        if calibration is None or not self._recent:
            return
        references = list(calibration.reference_pressure_hpa)
        drifts = list(calibration.pressure_drift_hpa)
        for index in range(BMP_COUNT):
            readings = [
                compensated_pressure_hpa(
                    packet, index, self._converter.bmp_calibrations[index]
                )[0]
                for packet in self._recent
            ]
            usable = [value for value in readings if not math.isnan(value)]
            if usable:
                references[index] = statistics.median(usable)
                drifts[index] = 0.0
        self._converter.calibration = RestCalibration(
            samples=calibration.samples,
            gyro_bias_dps=calibration.gyro_bias_dps,
            gravity_mps2=calibration.gravity_mps2,
            gravity_norm_mps2=calibration.gravity_norm_mps2,
            reference_pressure_hpa=tuple(references),
            pressure_drift_hpa=tuple(drifts),
        )

    def restart_calibration(self) -> None:
        self.calibrating = True
        self._calibration_packets = []
        self._calibration_elapsed_us = 0
        self._converter.calibration = None

    # ------------------------------------------------------------ data intake

    def _tick(self) -> None:
        packets = self._decoder.feed(self._source.read())
        for packet in packets:
            self._ingest(packet)
        if self.calibrating:
            self._update_calibration_label()
            return
        if not self.paused:
            self._redraw()

    def _ingest(self, packet: ImuPacket) -> None:
        if self._previous_timestamp_us is not None:
            delta = elapsed_us(self._previous_timestamp_us, packet.timestamp_us)
            self._elapsed_s += delta / 1_000_000.0
            if self.calibrating:
                self._calibration_elapsed_us += delta
        self._previous_timestamp_us = packet.timestamp_us

        self._recent.append(packet)
        limit = int(VERDICT_WINDOW_SECONDS * self._decoder.device.imu_rate_hz)
        if len(self._recent) > limit:
            del self._recent[:-limit]

        if self.calibrating:
            self._calibration_packets.append(packet)
            target_us = self._options.calibration * 1_000_000
            if self._calibration_elapsed_us >= target_us:
                self._finish_calibration()
            return

        self._buffer.append(
            self._elapsed_s,
            self._converter.imu_row(packet),
            self._converter.baro_row(packet),
        )

    def _finish_calibration(self) -> None:
        self._converter.calibration = calibrate_at_rest(
            self._calibration_packets,
            self._decoder.device,
            self._converter.bmp_calibrations,
        )
        self._converter.reset_gravity_tracking()
        self._calibration_packets = []
        self.calibrating = False

    # -------------------------------------------------------------- rendering

    def _series(
        self,
        times: np.ndarray,
        values: np.ndarray,
        filter_samples: int,
    ) -> tuple[np.ndarray, np.ndarray]:
        """Drop repeats, then smooth, in that order.

        The barometer answers at 25 Hz and the magnetometer at 20 Hz, but both
        are repeated from cache into every 100 Hz packet. Averaging the repeats
        would widen the window without adding a single new reading, so the
        duplicates go first.
        """
        finite = np.isfinite(values)
        if not finite.all():
            times = times[finite]
            values = values[finite]
        if values.size == 0:
            return times, values
        if self.decimate:
            mask = changed_mask(values)
            times = times[mask]
            values = values[mask]
        if filter_samples > 1:
            values = trailing_average(values, filter_samples)
        return times, values

    def _redraw(self) -> None:
        times, imu, baro = self._buffer.window(self.window_seconds)
        if times.size == 0:
            return

        imu_samples = (
            max(1, round(DEFAULT_IMU_FILTER_SECONDS * self._decoder.device.imu_rate_hz))
            if self.filter_imu
            else 1
        )
        for sensor in range(SENSOR_COUNT):
            for quantity in range(3):
                base = sensor * 9 + quantity * 3
                lowest = math.inf
                highest = -math.inf
                for axis in range(3):
                    axis_times, axis_values = self._series(
                        times, imu[:, base + axis], imu_samples
                    )
                    self._curves[sensor][quantity][axis].setData(
                        axis_times, axis_values
                    )
                    if axis_values.size:
                        lowest = min(lowest, float(axis_values.min()))
                        highest = max(highest, float(axis_values.max()))
                self._apply_scale(
                    self._plots[sensor][quantity],
                    self._scalers[(sensor, quantity)],
                    lowest,
                    highest,
                )

        pressure_samples = (
            max(
                1,
                round(
                    DEFAULT_PRESSURE_FILTER_SECONDS * self._decoder.device.bmp_rate_hz
                ),
            )
            if self.filter_pressure
            else 1
        )
        pressure_low, pressure_high = math.inf, -math.inf
        altitude_low, altitude_high = math.inf, -math.inf
        for index in range(BMP_COUNT):
            pressure_times, pressure = self._series(
                times, baro[:, COLUMN_PRESSURE + index], pressure_samples
            )
            self.pressure_curves[index].setData(pressure_times, pressure)
            if pressure.size:
                pressure_low = min(pressure_low, float(pressure.min()))
                pressure_high = max(pressure_high, float(pressure.max()))

            altitude_times, altitude = self._series(
                times, baro[:, COLUMN_ALTITUDE + index], pressure_samples
            )
            self.altitude_curves[index].setData(altitude_times, altitude)
            if altitude.size:
                altitude_low = min(altitude_low, float(altitude.min()))
                altitude_high = max(altitude_high, float(altitude.max()))
            self._latest_altitude[index] = (
                float(altitude[-1]) if altitude.size else math.nan
            )
        self._apply_scale(
            self.pressure_plot, self._pressure_scaler, pressure_low, pressure_high
        )
        self._apply_scale(
            self.altitude_plot, self._altitude_scaler, altitude_low, altitude_high
        )
        self._update_altitude_readout()
        self._update_footer(baro)

    def _apply_scale(
        self, plot, scaler: AxisScaler, lowest: float, highest: float
    ) -> None:
        if self.scale_locked or not math.isfinite(lowest) or not math.isfinite(highest):
            return
        low, high = scaler.update(lowest, highest)
        plot.setYRange(low, high, padding=0)

    def _update_altitude_readout(self) -> None:
        cells = []
        for index in range(BMP_COUNT):
            value = self._latest_altitude[index]
            colour = AXIS_COLORS[index]
            text = "--" if math.isnan(value) else f"{value:+0.0f}"
            cells.append(
                f"<span style='color:{colour}'>BMP{index}"
                f"<span style='font-size:30pt'><b> {text}</b></span> cm</span>"
            )
        self.altitude_readout.setText(
            "<span style='font-size:15pt'>Altitude &nbsp;&nbsp; "
            + "&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;".join(cells)
            + "</span>"
        )

    def _update_calibration_label(self) -> None:
        remaining = max(
            0.0,
            self._options.calibration - self._calibration_elapsed_us / 1_000_000.0,
        )
        self.status.setText(
            "<span style='font-size:16pt'>"
            "<b>Calibrando giroscopio e gravidade</b> &mdash; mantenha o "
            f"dispositivo parado &mdash; faltam {remaining:0.1f} s"
            "</span>"
        )

    def _refresh_verdicts(self) -> None:
        if self.calibrating or not self._recent:
            return
        verdicts, overall = evaluate_all(
            self._recent,
            self._decoder.stats,
            self._decoder.device,
            self._decoder.banner_bytes,
            at_rest=False,
        )
        parts = []
        for verdict in verdicts:
            colour = LEVEL_COLORS.get(verdict.level, "#333333")
            label = verdict.name.upper()
            # A bare level tells nobody what to do about it; the reason does.
            detail = verdict.level
            if verdict.reasons:
                detail = f"{verdict.level} ({', '.join(verdict.reasons)})"
            parts.append(
                f"<span style='color:{colour}'><b>{label}</b> {detail}</span>"
            )
        stream = verdicts[0].details
        summary = (
            f"taxa {stream.get('effective_rate_hz', '?')} Hz &middot; "
            f"perdas {stream.get('sequence_gaps', '?')} &middot; "
            f"crc {stream.get('crc_failures', '?')}"
        )
        state = "PAUSADO" if self.paused else "AO VIVO"
        overall_colour = LEVEL_COLORS.get(overall, "#333333")
        self._verdict_text = (
            f"<span style='font-size:13pt'><b>{state}</b> &middot; "
            f"<span style='color:{overall_colour}'>geral {overall}</span> "
            f"&middot; {summary} &middot; " + " &middot; ".join(parts) + "</span>"
        )
        self.status.setText(self._verdict_text)

    def _update_footer(self, baro: np.ndarray) -> None:
        latest = baro[-1]
        calibration = self._converter.calibration
        gravity = ""
        if calibration is not None and calibration.valid:
            norms = " ".join(
                f"ICM{index}={value:0.2f}"
                for index, value in enumerate(calibration.gravity_norm_mps2)
            )
            gravity = f"gravidade calibrada {norms} m/s2 &middot; "
        temperatures = " ".join(
            f"BMP{index}={latest[COLUMN_TEMPERATURE + index]:0.2f}C"
            for index in range(BMP_COUNT)
        )
        # The filtered value, never the last raw sample: a single unfiltered
        # reading swings by tens of centimetres and reads as a broken sensor.
        pressures = " ".join(
            f"BMP{index}={latest[COLUMN_ABSOLUTE_PRESSURE + index]:0.2f}hPa"
            for index in range(BMP_COUNT)
        )
        warnings = ""
        if calibration is not None and calibration.valid:
            references = " ".join(
                f"BMP{index}="
                + ("--" if value is None else f"{value:0.2f}")
                for index, value in enumerate(calibration.reference_pressure_hpa)
            )
            warnings = f"referencia {references} hPa &middot; "
            if not calibration.pressure_settled:
                warnings += (
                    "<span style='color:#b26a00'><b>barometro ainda assentava na "
                    "calibracao, use Z para re-zerar</b></span> &middot; "
                )
            elif not calibration.barometers_agree:
                warnings += (
                    "<span style='color:#b26a00'><b>referencias discordam, use Z"
                    "</b></span> &middot; "
                )
        states = " &middot; ".join(
            (
                f"gravidade {'removida (adaptativa)' if self.remove_gravity else 'presente'}",
                f"escala {'travada' if self.scale_locked else 'automatica'}",
                f"filtro barometro {'ligado' if self.filter_pressure else 'desligado'}",
                f"filtro IMU {'ligado' if self.filter_imu else 'desligado'}",
                f"repetidos {'ocultos' if self.decimate else 'visiveis'}",
                f"janela {self.window_seconds:0.0f}s",
            )
        )
        self.footer.setText(
            f"<span style='font-size:11pt'>{gravity}"
            f"pressao {pressures} &middot; "
            f"temperatura {temperatures}<br>{warnings}{states}<br>"
            "espaco pausa &middot; G gravidade &middot; L escala &middot; "
            "F filtro IMU &middot; B filtro barometro &middot; D repetidos "
            "&middot; Z re-zerar altitude &middot; R recalibrar &middot; "
            "setas janela &middot; Q sair</span>"
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--port", help="serial port, for example COM3; auto-detected if omitted"
    )
    parser.add_argument(
        "--window",
        type=float,
        default=DEFAULT_WINDOW_SECONDS,
        help="visible sliding window in seconds (default: 10)",
    )
    parser.add_argument(
        "--calibration",
        type=float,
        default=DEFAULT_CALIBRATION_SECONDS,
        help="still calibration window in seconds (default: 3)",
    )
    parser.add_argument(
        "--mag-calibration",
        type=Path,
        help="JSON produced by calibrate_magnetometer.py",
    )
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    if not MINIMUM_WINDOW_SECONDS <= args.window <= MAXIMUM_WINDOW_SECONDS:
        parser.error(
            f"--window must be between {MINIMUM_WINDOW_SECONDS:g} and "
            f"{MAXIMUM_WINDOW_SECONDS:g} seconds"
        )
    if args.calibration <= 0 or args.calibration > 30:
        parser.error("--calibration must be greater than 0 and at most 30 seconds")

    try:
        import pyqtgraph as pg
        from pyqtgraph.Qt import QtCore, QtGui, QtWidgets
    except ImportError as exc:  # pragma: no cover - matches analyze_imu.py
        raise SystemExit(
            "pyqtgraph and a Qt binding are required; install them with: "
            "pip install -r python/requirements.txt"
        ) from exc

    pg.setConfigOption("background", "w")
    pg.setConfigOption("foreground", "k")

    decoder = StreamDecoder()
    try:
        port_name = args.port or detect_port()
        print(f"port={port_name}")
        with serial.Serial(
            port_name, SERIAL_BAUD, timeout=READ_TIMEOUT_SECONDS
        ) as port:
            port.dtr = True
            port.rts = True
            time.sleep(0.2)
            port.reset_input_buffer()
            blocking = SerialSource(port)
            if not wait_for_banner(blocking, decoder, BANNER_TIMEOUT_SECONDS):
                raise TimeoutError(
                    "no firmware banner received; confirm that "
                    "kPresentationStreamEnabled is true in src/config/constants.h"
                )
            if not decoder.device.streaming:
                raise RuntimeError(
                    "the firmware reported streaming=0; run "
                    "python/presentation_monitor.py for the sensor report"
                )
            application = QtWidgets.QApplication.instance() or QtWidgets.QApplication(
                sys.argv
            )
            panel = PresentationPanel(
                pg,
                QtCore,
                QtGui,
                QtWidgets,
                SerialSource(port, block=False),
                decoder,
                args,
            )
            panel.widget.show()
            application.exec()
    except (OSError, RuntimeError, TimeoutError, serial.SerialException) as exc:
        print(f"panel_failed: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc


if __name__ == "__main__":
    main()
