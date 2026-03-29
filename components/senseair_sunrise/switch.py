import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID

from . import senseair_sunrise_ns, SenseairSunriseComponent

CONF_ABC = "abc"
CONF_SENSEAIR_SUNRISE_ID = "senseair_sunrise_id"

SenseairSunriseABCSwitch = senseair_sunrise_ns.class_(
    "SenseairSunriseABCSwitch", switch.Switch, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_SENSEAIR_SUNRISE_ID): cv.use_id(
            SenseairSunriseComponent
        ),
        cv.Optional(CONF_ABC): switch.switch_schema(
            SenseairSunriseABCSwitch,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_SENSEAIR_SUNRISE_ID])

    if abc_config := config.get(CONF_ABC):
        sw = await switch.new_switch(abc_config)
        await cg.register_component(sw, abc_config)
        cg.add(sw.set_parent(parent))
