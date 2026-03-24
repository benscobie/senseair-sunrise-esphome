import math
import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import i2c, sensor
from esphome.const import (
    CONF_CO2,
    CONF_ID,
    CONF_TEMPERATURE,
    DEVICE_CLASS_CARBON_DIOXIDE,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_PARTS_PER_MILLION,
    ICON_MOLECULE_CO2,
)

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = []
DEPENDENCIES = ["i2c"]

CONF_MEASUREMENT_MODE = "measurement_mode"
CONF_NUMBER_OF_SAMPLES = "number_of_samples"
CONF_MEASUREMENT_PERIOD = "measurement_period"
CONF_IIR_FILTER = "iir_filter"

MEASUREMENT_MODES = {
    "continuous": 0,
}

senseair_sunrise_ns = cg.esphome_ns.namespace("senseair_sunrise")
SenseairSunriseComponent = senseair_sunrise_ns.class_(
    "SenseairSunriseComponent", cg.PollingComponent, i2c.I2CDevice
)

def _min_measurement_period(samples):
    """Minimum measurement period: ceil(samples * 0.3) rounded up to nearest even second."""
    min_s = math.ceil(samples * 0.3)
    if min_s % 2 != 0:
        min_s += 1
    return max(min_s, 2)


def _validate_tuning(config):
    samples = config[CONF_NUMBER_OF_SAMPLES]
    min_period = _min_measurement_period(samples)

    if CONF_MEASUREMENT_PERIOD in config:
        total_ms = int(config[CONF_MEASUREMENT_PERIOD].total_milliseconds)
        if total_ms % 1000 != 0:
            raise cv.Invalid("measurement_period must be a whole number of seconds")
        period_s = total_ms // 1000
        if period_s < min_period:
            raise cv.Invalid(
                f"measurement_period ({period_s}s) too short for {samples} samples. "
                f"Minimum: {min_period}s (samples x 300ms, rounded to even seconds)"
            )
        config[CONF_MEASUREMENT_PERIOD] = period_s
    else:
        config[CONF_MEASUREMENT_PERIOD] = min_period

    period = config[CONF_MEASUREMENT_PERIOD]
    if CONF_IIR_FILTER not in config:
        if period > 60:
            _LOGGER.info(
                "Measurement period > 60s: auto-disabling IIR filter "
                "(recommended per Senseair documentation)"
            )
            config[CONF_IIR_FILTER] = False
        else:
            config[CONF_IIR_FILTER] = True
    elif config[CONF_IIR_FILTER] and period > 60:
        _LOGGER.warning(
            "IIR filter enabled with measurement period > 60s. "
            "Senseair recommends disabling IIR and increasing samples for long periods."
        )

    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SenseairSunriseComponent),
            cv.Optional(CONF_MEASUREMENT_MODE, default="continuous"): cv.enum(
                MEASUREMENT_MODES, lower=True
            ),
            cv.Optional(CONF_NUMBER_OF_SAMPLES, default=8): cv.int_range(
                min=1, max=1024
            ),
            cv.Optional(CONF_MEASUREMENT_PERIOD): cv.positive_time_period,
            cv.Optional(CONF_IIR_FILTER): cv.boolean,
            cv.Optional(CONF_CO2): sensor.sensor_schema(
                unit_of_measurement=UNIT_PARTS_PER_MILLION,
                icon=ICON_MOLECULE_CO2,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_CARBON_DIOXIDE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
        }
    )
    .extend(cv.polling_component_schema("60s"))
    .extend(i2c.i2c_device_schema(0x68)),
    _validate_tuning,
)


BackgroundCalibrationAction = senseair_sunrise_ns.class_(
    "BackgroundCalibrationAction", automation.Action
)
ABCEnableAction = senseair_sunrise_ns.class_("ABCEnableAction", automation.Action)
ABCDisableAction = senseair_sunrise_ns.class_("ABCDisableAction", automation.Action)

SENSEAIR_SUNRISE_ACTION_SCHEMA = automation.maybe_simple_id(
    {cv.GenerateID(): cv.use_id(SenseairSunriseComponent)}
)


@automation.register_action(
    "senseair_sunrise.background_calibration",
    BackgroundCalibrationAction,
    SENSEAIR_SUNRISE_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "senseair_sunrise.abc_enable",
    ABCEnableAction,
    SENSEAIR_SUNRISE_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "senseair_sunrise.abc_disable",
    ABCDisableAction,
    SENSEAIR_SUNRISE_ACTION_SCHEMA,
    synchronous=True,
)
async def senseair_sunrise_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    cg.add(var.set_measurement_mode(config[CONF_MEASUREMENT_MODE]))
    cg.add(var.set_number_of_samples(config[CONF_NUMBER_OF_SAMPLES]))
    cg.add(var.set_measurement_period(config[CONF_MEASUREMENT_PERIOD]))
    cg.add(var.set_iir_filter(config[CONF_IIR_FILTER]))

    if co2_config := config.get(CONF_CO2):
        sens = await sensor.new_sensor(co2_config)
        cg.add(var.set_co2_sensor(sens))

    if temp_config := config.get(CONF_TEMPERATURE):
        sens = await sensor.new_sensor(temp_config)
        cg.add(var.set_temperature_sensor(sens))
