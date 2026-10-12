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
CONF_PAGE_ID = "page_id"
CONF_ROOT_PATH = "root_path"
CONF_SCALE = "scale"
CONF_DITHER = "dither"
CONF_AUTO_ROTATE = "auto_rotate"
CONF_COLOR_SPACE = "color_space"
CONF_PALETTE = "palette"
CONF_ERROR_CLAMP = "error_clamp"
CONF_DITHER_STRENGTH = "dither_strength"
CONF_TONE_COMPRESSION = "tone_compression"
CONF_TONE_CURVE = "tone_curve"
CONF_SATURATION = "saturation"

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

DitherMethod = ns.enum("DitherMethod")
DITHER_METHODS = {
    "none": DitherMethod.DITHER_NONE,
    "floyd_steinberg": DitherMethod.DITHER_FLOYD_STEINBERG,
    "atkinson": DitherMethod.DITHER_ATKINSON,  # only diffuses 3/4 of the error: more contrast, less noise
    "jarvis_judice_ninke": DitherMethod.DITHER_JJN,  # wider kernel: smoother, less wormy
    "dizzy": DitherMethod.DITHER_DIZZY,  # random pixel order: no directional artefacts, grainier
}


def _dither(value):
    # keep `dither: true/false` working
    if isinstance(value, bool):
        value = "floyd_steinberg" if value else "none"
    return cv.enum(DITHER_METHODS, lower=True)(value)


ToneCurve = ns.enum("ToneCurve")
TONE_CURVES = {
    "linear": ToneCurve.TONE_LINEAR,  # scale the whole range: max highlight detail, flatter image
    "soft": ToneCurve.TONE_SOFT,  # keep midtones, squeeze only the ends
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
        # page whose lambda calls id(gallery).draw(it); shown automatically for each new image
        cv.Optional(CONF_PAGE_ID): cv.use_id(display.DisplayPage),
        cv.Optional(CONF_ROOT_PATH, default="/sdcard/images"): cv.string,
        cv.Optional(CONF_SCALE, default="fit"): cv.enum(SCALE_MODES, lower=True),
        cv.Optional(CONF_DITHER, default="floyd_steinberg"): _dither,
        cv.Optional(CONF_ERROR_CLAMP, default=64): cv.int_range(min=0, max=255),
        cv.Optional(CONF_DITHER_STRENGTH, default="100%"): cv.percentage,
        cv.Optional(CONF_TONE_COMPRESSION, default="100%"): cv.percentage,
        cv.Optional(CONF_TONE_CURVE, default="linear"): cv.enum(TONE_CURVES, lower=True),
        cv.Optional(CONF_SATURATION, default=1.3): cv.float_range(min=0.0, max=3.0),
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
    if CONF_PAGE_ID in config:
        page = await cg.get_variable(config[CONF_PAGE_ID])
        cg.add(var.set_page(page))
    cg.add(var.set_root_path(config[CONF_ROOT_PATH]))
    cg.add(var.set_scale_mode(config[CONF_SCALE]))
    cg.add(var.set_dither_method(config[CONF_DITHER]))
    cg.add(var.set_error_clamp(config[CONF_ERROR_CLAMP]))
    cg.add(var.set_dither_strength(config[CONF_DITHER_STRENGTH]))
    cg.add(var.set_tone_compression(config[CONF_TONE_COMPRESSION]))
    cg.add(var.set_tone_curve(config[CONF_TONE_CURVE]))
    cg.add(var.set_saturation(config[CONF_SATURATION]))
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
