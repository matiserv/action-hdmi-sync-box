import esphome.codegen as cg
from esphome.components import light
import esphome.config_validation as cv
from esphome.const import CONF_MAX_REFRESH_RATE, CONF_NUM_LEDS, CONF_OUTPUT_ID

from . import CONF_SYNCBOX_ID, SyncBox, syncbox_ns

DEPENDENCIES = ["syncbox"]

SyncBoxLight = syncbox_ns.class_("SyncBoxLight", light.AddressableLight)

CONFIG_SCHEMA = light.ADDRESSABLE_LIGHT_SCHEMA.extend(
    {
        cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(SyncBoxLight),
        cv.GenerateID(CONF_SYNCBOX_ID): cv.use_id(SyncBox),
        # 0 = suma bokow z syncbox.layout
        cv.Optional(CONF_NUM_LEDS, default=0): cv.int_range(min=0, max=256),
        cv.Optional(CONF_MAX_REFRESH_RATE): cv.positive_time_period_microseconds,
        # dithering w czasie dla ciemnych kolorow w trybie wideo (poziomy posrednie miedzy 1, 2, 3...)
        cv.Optional("dithering", default=True): cv.boolean,
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await light.register_light(var, config)
    await cg.register_component(var, config)
    hub = await cg.get_variable(config[CONF_SYNCBOX_ID])
    cg.add(var.set_parent(hub))
    cg.add(hub.set_light(var))
    cg.add(var.set_num_leds(config[CONF_NUM_LEDS]))
    cg.add(var.set_dithering(config["dithering"]))
    if CONF_MAX_REFRESH_RATE in config:
        cg.add(var.set_max_refresh_rate(config[CONF_MAX_REFRESH_RATE]))
