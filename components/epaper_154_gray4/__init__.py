from esphome import automation
import esphome.codegen as cg
from esphome.components import display, spi
import esphome.config_validation as cv
from esphome.const import CONF_ID

epaper_154_gray4_ns = cg.esphome_ns.namespace("epaper_154_gray4")
EPaper154Gray4 = epaper_154_gray4_ns.class_(
    "EPaper154Gray4", cg.PollingComponent, spi.SPIDevice, display.DisplayBuffer
)
FullUpdateNextAction = epaper_154_gray4_ns.class_(
    "FullUpdateNextAction", automation.Action
)


@automation.register_action(
    "epaper_154_gray4.full_update_next",
    FullUpdateNextAction,
    automation.maybe_simple_id({cv.GenerateID(): cv.use_id(EPaper154Gray4)}),
    synchronous=True,
)
async def full_update_next_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
