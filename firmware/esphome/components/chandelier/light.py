"""ESPHome light platform for the chandelier bridge: an on/off-only light.

Each light is bound to a hub and a 4-byte lamp ID. Turning it on/off transmits
the ON (05) / OFF (09) command for that ID. State is optimistic (one-way radio).
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.const import CONF_OUTPUT_ID

from . import ChandelierHub, chandelier_ns

DEPENDENCIES = ["chandelier"]

CONF_CHANDELIER_ID = "chandelier_id"
CONF_LAMP_ID = "lamp_id"

ChandelierLight = chandelier_ns.class_(
    "ChandelierLight", cg.Component, light.LightOutput
)

CONFIG_SCHEMA = light.LIGHT_SCHEMA.extend(
    {
        cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(ChandelierLight),
        cv.GenerateID(CONF_CHANDELIER_ID): cv.use_id(ChandelierHub),
        cv.Required(CONF_LAMP_ID): cv.All(
            [cv.hex_uint8_t], cv.Length(min=4, max=4)
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await cg.register_component(var, config)
    await light.register_light(var, config)

    hub = await cg.get_variable(config[CONF_CHANDELIER_ID])
    cg.add(var.set_hub(hub))
    lid = config[CONF_LAMP_ID]
    cg.add(var.set_lamp_id(lid[0], lid[1], lid[2], lid[3]))
