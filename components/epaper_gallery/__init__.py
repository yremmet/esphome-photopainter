import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import display
from esphome.const import CONF_ID
from esphome.core import CORE

try:
    from esphome.components.esp32 import require_vfs_dir
except ImportError:
    require_vfs_dir = None

DEPENDENCIES = ["display"]

CONF_DISPLAY_ID = "display_id"
CONF_ROOT_PATH = "root_path"
CONF_SCALE = "scale"
CONF_DITHER = "dither"
CONF_AUTO_ROTATE = "auto_rotate"
CONF_COLOR_SPACE = "color_space"
CONF_PALETTE = "palette"

ns = cg.esphome_ns.namespace("epaper_gallery")
EPaperGallery = ns.class_("EPaperGallery", cg.Component)
ScaleMode = ns.enum("ScaleMode")
SCALE_MODES = {
    "fit": ScaleMode.SCALE_FIT,    # letterbox, white borders
    "fill": ScaleMode.SCALE_FILL,  # cover, centered crop
}
ColorSpace = ns.enum("ColorSpace")
COLOR_SPACES = {
    "rgb": ColorSpace.CS_RGB,  # plain euclidean distance in RGB
    "lab": ColorSpace.CS_LAB,  # perceptual CIELAB distance (recommended)
}

# Calibrated panel colours, order fixed: black, white, yellow, red, blue, green
PALETTE_ORDER = ["black", "white", "yellow", "red", "blue", "green"]


def _hex_color(value):
    value = cv.string_strict(value).strip().lstrip("#")
    if len(value) != 6:
        raise cv.Invalid("Colour must be RRGGBB hex, e.g. '#C02828'")
    try:
        return [int(value[i : i + 2], 16) for i in (0, 2, 4)]
    except ValueError as e:
        raise cv.Invalid("Colour must be RRGGBB hex") from e


PALETTE_SCHEMA = cv.Schema({cv.Required(name): _hex_color for name in PALETTE_ORDER})

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(EPaperGallery),
        cv.Required(CONF_DISPLAY_ID): cv.use_id(display.Display),
        cv.Optional(CONF_ROOT_PATH, default="/sdcard/images"): cv.string,
        cv.Optional(CONF_SCALE, default="fit"): cv.enum(SCALE_MODES, lower=True),
        cv.Optional(CONF_DITHER, default=True): cv.boolean,
        cv.Optional(CONF_AUTO_ROTATE, default=True): cv.boolean,
        cv.Optional(CONF_COLOR_SPACE, default="lab"): cv.enum(COLOR_SPACES, lower=True),
        cv.Optional(CONF_PALETTE): PALETTE_SCHEMA,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    disp = await cg.get_variable(config[CONF_DISPLAY_ID])
    cg.add(var.set_display(disp))
    cg.add(var.set_root_path(config[CONF_ROOT_PATH]))
    cg.add(var.set_scale_mode(config[CONF_SCALE]))
    cg.add(var.set_dither(config[CONF_DITHER]))
    cg.add(var.set_auto_rotate(config[CONF_AUTO_ROTATE]))
    cg.add(var.set_color_space(config[CONF_COLOR_SPACE]))
    if CONF_PALETTE in config:
        for i, name in enumerate(PALETTE_ORDER):
            r, g, b = config[CONF_PALETTE][name]
            cg.add(var.set_match_color(i, r, g, b))
    if require_vfs_dir is not None and CORE.is_esp32:
        require_vfs_dir()  # opendir/readdir on the SD card
    # Same JPEG decoder ESPHome itself uses for online_image / runtime_image
    cg.add_library("JPEGDEC", None, "https://github.com/bitbank2/JPEGDEC#1.8.2")
