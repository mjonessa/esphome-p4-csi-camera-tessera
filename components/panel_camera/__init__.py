"""Panel camera: the plugin's registration with Tessera. The camera itself is p4_csi_camera beside this folder
(plugin.yaml); register_plugin() takes the id, version and texts from the manifest and translations/."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import smart_display
from esphome.const import CONF_ID

DEPENDENCIES = ["smart_display", "p4_csi_camera"]

panel_camera_ns = cg.esphome_ns.namespace("panel_camera")
PanelCamera = panel_camera_ns.class_("PanelCamera", cg.Component)

CONFIG_SCHEMA = cv.Schema({cv.GenerateID(): cv.declare_id(PanelCamera)}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await smart_display.register_plugin(var, __file__)
