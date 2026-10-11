"""Tests for senseair_sunrise sensor.py validation logic."""

from conftest import FakeTimePeriod
import pytest

from components.senseair_sunrise.sensor import (
    CONF_ABC_PERIOD,
    CONF_ALTITUDE,
    CONF_IIR_FILTER,
    CONF_MEASUREMENT_MODE,
    CONF_MEASUREMENT_PERIOD,
    CONF_NUMBER_OF_SAMPLES,
    CONF_PRESSURE,
    CONF_PRESSURE_SOURCE,
    _min_measurement_period,
    _validate_tuning,
)
import esphome.config_validation as cv

# ---------------------------------------------------------------------------
# _min_measurement_period
# ---------------------------------------------------------------------------


class TestMinMeasurementPeriod:
    """Tests for _min_measurement_period(samples)."""

    def test_1_sample(self):
        # ceil(1 * 0.3) = 1 -> odd, round up to 2
        assert _min_measurement_period(1) == 2

    def test_8_samples_default(self):
        # ceil(8 * 0.3) = ceil(2.4) = 3 -> odd, round up to 4
        assert _min_measurement_period(8) == 4

    def test_10_samples(self):
        # ceil(10 * 0.3) = 3 -> odd, round up to 4
        assert _min_measurement_period(10) == 4

    def test_20_samples_even_result(self):
        # ceil(20 * 0.3) = 6 -> already even
        assert _min_measurement_period(20) == 6

    def test_1024_samples_max(self):
        # ceil(1024 * 0.3) = ceil(307.2) = 308 -> even
        assert _min_measurement_period(1024) == 308

    def test_floor_is_2(self):
        # Even for tiny sample counts the minimum is 2
        assert _min_measurement_period(1) >= 2

    @pytest.mark.parametrize("samples", [1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024])
    def test_always_even(self, samples):
        result = _min_measurement_period(samples)
        assert result % 2 == 0, f"Expected even, got {result} for {samples} samples"

    @pytest.mark.parametrize("samples", [1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024])
    def test_at_least_2(self, samples):
        assert _min_measurement_period(samples) >= 2


# ---------------------------------------------------------------------------
# _validate_tuning — pressure compensation mutual exclusivity
# ---------------------------------------------------------------------------


class TestPressureCompensation:
    """Pressure / altitude / pressure_source are mutually exclusive."""

    def test_pressure_only(self, continuous_config):
        continuous_config[CONF_PRESSURE] = 1013.25
        result = _validate_tuning(continuous_config)
        assert result[CONF_PRESSURE] == 1013.25

    def test_altitude_converts_to_pressure(self, continuous_config):
        continuous_config[CONF_ALTITUDE] = 0
        result = _validate_tuning(continuous_config)
        assert CONF_ALTITUDE not in result
        assert result[CONF_PRESSURE] == pytest.approx(1013.25, abs=0.1)

    def test_altitude_1000m(self, continuous_config):
        continuous_config[CONF_ALTITUDE] = 1000
        result = _validate_tuning(continuous_config)
        expected = 1013.25 * (1 - 2.25577e-5 * 1000) ** 5.25588
        assert result[CONF_PRESSURE] == pytest.approx(expected, abs=0.1)

    def test_pressure_and_altitude_rejected(self, continuous_config):
        continuous_config[CONF_PRESSURE] = 1013.25
        continuous_config[CONF_ALTITUDE] = 100
        with pytest.raises(cv.Invalid, match="Only one of"):
            _validate_tuning(continuous_config)

    def test_pressure_and_source_rejected(self, continuous_config):
        continuous_config[CONF_PRESSURE] = 1013.25
        continuous_config[CONF_PRESSURE_SOURCE] = "some_sensor"
        with pytest.raises(cv.Invalid, match="Only one of"):
            _validate_tuning(continuous_config)

    def test_all_three_rejected(self, continuous_config):
        continuous_config[CONF_PRESSURE] = 1013.25
        continuous_config[CONF_ALTITUDE] = 100
        continuous_config[CONF_PRESSURE_SOURCE] = "some_sensor"
        with pytest.raises(cv.Invalid, match="Only one of"):
            _validate_tuning(continuous_config)


# ---------------------------------------------------------------------------
# _validate_tuning — ABC period
# ---------------------------------------------------------------------------


class TestABCPeriod:
    """ABC period must be whole hours in 1–65534 range (0 and 65535 disable ABC)."""

    def test_valid_180h(self, continuous_config):
        continuous_config[CONF_ABC_PERIOD] = FakeTimePeriod(180 * 3600000)
        result = _validate_tuning(continuous_config)
        assert result[CONF_ABC_PERIOD] == 180

    def test_1h_minimum(self, continuous_config):
        continuous_config[CONF_ABC_PERIOD] = FakeTimePeriod(1 * 3600000)
        result = _validate_tuning(continuous_config)
        assert result[CONF_ABC_PERIOD] == 1

    def test_65534h_maximum(self, continuous_config):
        continuous_config[CONF_ABC_PERIOD] = FakeTimePeriod(65534 * 3600000)
        result = _validate_tuning(continuous_config)
        assert result[CONF_ABC_PERIOD] == 65534

    def test_65535h_rejected(self, continuous_config):
        # TDE5531: writing 65535 to the ABC period disables ABC
        continuous_config[CONF_ABC_PERIOD] = FakeTimePeriod(65535 * 3600000)
        with pytest.raises(cv.Invalid, match="out of range"):
            _validate_tuning(continuous_config)

    def test_0h_rejected(self, continuous_config):
        continuous_config[CONF_ABC_PERIOD] = FakeTimePeriod(0)
        with pytest.raises(cv.Invalid, match="out of range"):
            _validate_tuning(continuous_config)

    def test_65536h_rejected(self, continuous_config):
        continuous_config[CONF_ABC_PERIOD] = FakeTimePeriod(65536 * 3600000)
        with pytest.raises(cv.Invalid, match="out of range"):
            _validate_tuning(continuous_config)

    def test_non_whole_hour_rejected(self, continuous_config):
        # 1.5 hours = 5400000 ms
        continuous_config[CONF_ABC_PERIOD] = FakeTimePeriod(5400000)
        with pytest.raises(cv.Invalid, match="whole number of hours"):
            _validate_tuning(continuous_config)


# ---------------------------------------------------------------------------
# _validate_tuning — single mode
# ---------------------------------------------------------------------------


class TestSingleMode:
    """Single-mode specific validation."""

    def test_measurement_period_rejected(self, single_config):
        single_config[CONF_MEASUREMENT_PERIOD] = FakeTimePeriod(16000)
        with pytest.raises(cv.Invalid, match="not used in single"):
            _validate_tuning(single_config)

    def test_iir_auto_disabled(self, single_config):
        result = _validate_tuning(single_config)
        assert result[CONF_IIR_FILTER] is False

    def test_iir_explicit_true_preserved(self, single_config):
        single_config[CONF_IIR_FILTER] = True
        result = _validate_tuning(single_config)
        assert result[CONF_IIR_FILTER] is True

    def test_period_set_to_zero(self, single_config):
        result = _validate_tuning(single_config)
        assert result[CONF_MEASUREMENT_PERIOD] == 0


# ---------------------------------------------------------------------------
# _validate_tuning — continuous mode measurement period
# ---------------------------------------------------------------------------


class TestContinuousModePeriod:
    """Continuous-mode measurement period validation."""

    def test_default_period_16s(self, continuous_config):
        result = _validate_tuning(continuous_config)
        assert result[CONF_MEASUREMENT_PERIOD] == 16

    def test_default_period_uses_min_if_higher(self):
        # 1024 samples -> min period 308s, which is > 16
        config = {
            CONF_MEASUREMENT_MODE: 0,
            CONF_NUMBER_OF_SAMPLES: 1024,
        }
        result = _validate_tuning(config)
        assert result[CONF_MEASUREMENT_PERIOD] == 308

    def test_explicit_16s(self, continuous_config):
        continuous_config[CONF_MEASUREMENT_PERIOD] = FakeTimePeriod(16000)
        result = _validate_tuning(continuous_config)
        assert result[CONF_MEASUREMENT_PERIOD] == 16

    def test_odd_period_rounded_up(self, continuous_config):
        # 15s -> rounded to 16s
        continuous_config[CONF_MEASUREMENT_PERIOD] = FakeTimePeriod(15000)
        result = _validate_tuning(continuous_config)
        assert result[CONF_MEASUREMENT_PERIOD] == 16

    def test_period_too_short_rejected(self, continuous_config):
        # 8 samples -> min 4s; 2s is too short
        continuous_config[CONF_MEASUREMENT_PERIOD] = FakeTimePeriod(2000)
        with pytest.raises(cv.Invalid, match="too short"):
            _validate_tuning(continuous_config)

    def test_period_exceeds_max_rejected(self, continuous_config):
        continuous_config[CONF_MEASUREMENT_PERIOD] = FakeTimePeriod(70000 * 1000)
        with pytest.raises(cv.Invalid, match="exceeds sensor maximum"):
            _validate_tuning(continuous_config)

    def test_non_whole_seconds_rejected(self, continuous_config):
        continuous_config[CONF_MEASUREMENT_PERIOD] = FakeTimePeriod(16500)
        with pytest.raises(cv.Invalid, match="whole number of seconds"):
            _validate_tuning(continuous_config)

    def test_exact_min_period_accepted(self, continuous_config):
        # 8 samples -> min 4s
        continuous_config[CONF_MEASUREMENT_PERIOD] = FakeTimePeriod(4000)
        result = _validate_tuning(continuous_config)
        assert result[CONF_MEASUREMENT_PERIOD] == 4


# ---------------------------------------------------------------------------
# _validate_tuning — IIR filter auto-management
# ---------------------------------------------------------------------------


class TestIIRFilter:
    """IIR filter auto-enable/disable in continuous mode."""

    def test_auto_enabled_short_period(self, continuous_config):
        continuous_config[CONF_MEASUREMENT_PERIOD] = FakeTimePeriod(16000)
        result = _validate_tuning(continuous_config)
        assert result[CONF_IIR_FILTER] is True

    def test_auto_disabled_long_period(self, continuous_config):
        continuous_config[CONF_MEASUREMENT_PERIOD] = FakeTimePeriod(120000)
        result = _validate_tuning(continuous_config)
        assert result[CONF_IIR_FILTER] is False

    def test_boundary_60s_enabled(self, continuous_config):
        continuous_config[CONF_MEASUREMENT_PERIOD] = FakeTimePeriod(60000)
        result = _validate_tuning(continuous_config)
        assert result[CONF_IIR_FILTER] is True

    def test_boundary_62s_disabled(self, continuous_config):
        continuous_config[CONF_MEASUREMENT_PERIOD] = FakeTimePeriod(62000)
        result = _validate_tuning(continuous_config)
        assert result[CONF_IIR_FILTER] is False

    def test_explicit_override_preserved(self, continuous_config):
        continuous_config[CONF_MEASUREMENT_PERIOD] = FakeTimePeriod(120000)
        continuous_config[CONF_IIR_FILTER] = True  # user overrides
        result = _validate_tuning(continuous_config)
        assert result[CONF_IIR_FILTER] is True
