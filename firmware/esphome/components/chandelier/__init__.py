"""ESPHome external component: XN297L chandelier RF bridge (hub).

The hub owns the nRF24L01+ radio and exposes the transmit routine. Lights and
buttons reference it; the light platform is in light.py, buttons are plain
`template` buttons in YAML calling `id(hub).send(...)`.
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@chandelier-bridge"]
MULTI_CONF = False

chandelier_ns = cg.esphome_ns.namespace("chandelier")
ChandelierHub = chandelier_ns.class_("ChandelierHub", cg.Component)

CONF_CE_PIN = "ce_pin"
CONF_CS_PIN = "cs_pin"
CONF_SCK_PIN = "sck_pin"
CONF_MOSI_PIN = "mosi_pin"
CONF_MISO_PIN = "miso_pin"
CONF_FRAMES_PER_BURST = "frames_per_burst"

# Plain GPIO numbers (Arduino pin numbering) — the component drives the nRF24
# over the Arduino SPI library directly, same as the proven sniffer firmware.
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ChandelierHub),
        cv.Optional(CONF_CE_PIN, default=9): cv.int_range(min=0, max=48),
        cv.Optional(CONF_CS_PIN, default=10): cv.int_range(min=0, max=48),
        cv.Optional(CONF_SCK_PIN, default=12): cv.int_range(min=0, max=48),
        cv.Optional(CONF_MOSI_PIN, default=11): cv.int_range(min=0, max=48),
        cv.Optional(CONF_MISO_PIN, default=13): cv.int_range(min=0, max=48),
        cv.Optional(CONF_FRAMES_PER_BURST, default=30): cv.int_range(min=1, max=60),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    # The component drives the nRF24 via the Arduino SPI library. Under the
    # ESP-IDF build (Arduino as a managed component) SPI is a separate library
    # whose include dir isn't on the path unless we declare it here. Arduino.h
    # itself is already available; only <SPI.h> needs this.
    cg.add_library("SPI", None)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_ce_pin(config[CONF_CE_PIN]))
    cg.add(var.set_cs_pin(config[CONF_CS_PIN]))
    cg.add(var.set_sck_pin(config[CONF_SCK_PIN]))
    cg.add(var.set_mosi_pin(config[CONF_MOSI_PIN]))
    cg.add(var.set_miso_pin(config[CONF_MISO_PIN]))
    cg.add(var.set_frames_per_burst(config[CONF_FRAMES_PER_BURST]))
