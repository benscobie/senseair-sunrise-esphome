"""Shared test fixtures for senseair_sunrise tests."""

import pytest

from components.senseair_sunrise.sensor import (
    CONF_MEASUREMENT_MODE,
    CONF_NUMBER_OF_SAMPLES,
)


class FakeTimePeriod:
    """Mimics esphome.config_validation.TimePeriod for testing."""

    def __init__(self, total_milliseconds):
        self.total_milliseconds = total_milliseconds


@pytest.fixture
def continuous_config():
    """Return a minimal valid config dict for continuous mode."""
    return {
        CONF_MEASUREMENT_MODE: 0,  # continuous
        CONF_NUMBER_OF_SAMPLES: 8,
    }


@pytest.fixture
def single_config():
    """Return a minimal valid config dict for single mode."""
    return {
        CONF_MEASUREMENT_MODE: 1,  # single
        CONF_NUMBER_OF_SAMPLES: 8,
    }
