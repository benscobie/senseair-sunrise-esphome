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

CODEOWNERS = []
DEPENDENCIES = ["i2c"]

CONF_MEASUREMENT_MODE = "measurement_mode"

MEASUREMENT_MODES = {
    "continuous": 0,
    "single": 1,
}

senseair_sunrise_ns = cg.esphome_ns.namespace("senseair_sunrise")
SenseairSunriseComponent = senseair_sunrise_ns.class_(
    "SenseairSunriseComponent", cg.PollingComponent, i2c.I2CDevice
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SenseairSunriseComponent),
            cv.Optional(CONF_MEASUREMENT_MODE, default="continuous"): cv.enum(
                MEASUREMENT_MODES, lower=True
            ),
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
    .extend(i2c.i2c_device_schema(0x68))
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
)
@automation.register_action(
    "senseair_sunrise.abc_enable",
    ABCEnableAction,
    SENSEAIR_SUNRISE_ACTION_SCHEMA,
)
@automation.register_action(
    "senseair_sunrise.abc_disable",
    ABCDisableAction,
    SENSEAIR_SUNRISE_ACTION_SCHEMA,
)
async def senseair_sunrise_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    cg.add(var.set_measurement_mode(config[CONF_MEASUREMENT_MODE]))

    if co2_config := config.get(CONF_CO2):
        sens = await sensor.new_sensor(co2_config)
        cg.add(var.set_co2_sensor(sens))

    if temp_config := config.get(CONF_TEMPERATURE):
        sens = await sensor.new_sensor(temp_config)
        cg.add(var.set_temperature_sensor(sens))
