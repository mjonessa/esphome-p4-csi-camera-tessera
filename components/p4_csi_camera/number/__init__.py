"""Number platform for P4 CSI Camera: JPEG quality, exposure, gains and the auto exposure target."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import ENTITY_CATEGORY_CONFIG, UNIT_MILLISECOND

from .. import p4_csi_camera_ns, P4CSICamera

DEPENDENCIES = ["p4_csi_camera"]

CONF_CAMERA_ID = "camera_id"

P4CSICameraNumber = p4_csi_camera_ns.class_("P4CSICameraNumber", number.Number, cg.Component)
NumberKind = p4_csi_camera_ns.enum("NumberKind")

# key: (kind, icon, unit, min, max, step). Exposure and the gains turn auto exposure off when set by hand.
NUMBERS = {
    "jpeg_quality": (NumberKind.NUMBER_JPEG_QUALITY, "mdi:quality-high", None, 10, 100, 1),
    "exposure": (NumberKind.NUMBER_EXPOSURE, "mdi:camera-timer", UNIT_MILLISECOND, 0.1, 1000, 0.1),
    "analog_gain": (NumberKind.NUMBER_ANALOG_GAIN, "mdi:brightness-6", "x", 1, 15.5, 0.0625),
    "digital_gain": (NumberKind.NUMBER_DIGITAL_GAIN, "mdi:brightness-7", "x", 1, 4, 0.05),
    "ae_target": (NumberKind.NUMBER_AE_TARGET, "mdi:brightness-auto", None, 16, 240, 1),
}


def _schema(icon, unit):
    extra = {"unit_of_measurement": unit} if unit else {}
    return number.number_schema(
        P4CSICameraNumber, entity_category=ENTITY_CATEGORY_CONFIG, icon=icon, **extra
    ).extend(cv.COMPONENT_SCHEMA)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_CAMERA_ID): cv.use_id(P4CSICamera),
        **{cv.Optional(key): _schema(icon, unit) for key, (_, icon, unit, *_rest) in NUMBERS.items()},
    }
)


async def to_code(config):
    camera = await cg.get_variable(config[CONF_CAMERA_ID])
    for key, (kind, _icon, _unit, lo, hi, step) in NUMBERS.items():
        if key not in config:
            continue
        conf = config[key]
        var = await number.new_number(conf, min_value=lo, max_value=hi, step=step)
        await cg.register_component(var, conf)
        cg.add(var.set_camera(camera))
        cg.add(var.set_kind(kind))
