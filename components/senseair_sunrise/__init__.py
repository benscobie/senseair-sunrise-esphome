import esphome.codegen as cg

CODEOWNERS = []
DEPENDENCIES = ["i2c"]

senseair_sunrise_ns = cg.esphome_ns.namespace("senseair_sunrise")
SenseairSunriseComponent = senseair_sunrise_ns.class_(
    "SenseairSunriseComponent", cg.PollingComponent
)
