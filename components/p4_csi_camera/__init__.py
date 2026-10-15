"""ESPHome ESP32-P4 MIPI CSI Camera Component."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_RESOLUTION
from esphome.components import i2c
from esphome.components.esp32 import add_idf_sdkconfig_option, add_idf_component

try:
    from esphome.cpp_helpers import setup_entity
except ImportError:
    from esphome.core.entity_helpers import setup_entity

CODEOWNERS = ["@sullb"]
DEPENDENCIES = ["esp32"]

# The OV02C10 sits on the same I2C wires as the touch panel (GPIO7/GPIO8). Pointing i2c_id at the ESPHome bus
# makes esp_video reuse that bus instead of creating a second controller on the same pins.
ESP_VIDEO_REF = "ee31057dcf3c652284959283780f591e3d6320f2"
AUTO_LOAD = ["camera", "psram"]
MULTI_CONF = False

CONF_DATA_LANES = "data_lanes"
CONF_MAX_FRAMERATE = "max_framerate"
CONF_IDLE_FRAMERATE = "idle_framerate"
CONF_RESET_PIN = "reset_pin"
CONF_POWER_PIN = "power_pin"
CONF_I2C_ID = "i2c_id"
CONF_SCCB_FREQUENCY = "sccb_frequency"
CONF_AUTO_EXPOSURE = "auto_exposure"
CONF_AE_TARGET = "ae_target"
CONF_MAX_EXPOSURE = "max_exposure"
CONF_FLICKER_FREQUENCY = "flicker_frequency"
CONF_JPEG_QUALITY = "jpeg_quality"
CONF_VERTICAL_FLIP = "vertical_flip"
CONF_HORIZONTAL_FLIP = "horizontal_flip"

p4_csi_camera_ns = cg.esphome_ns.namespace("p4_csi_camera")
P4CSICamera = p4_csi_camera_ns.class_(
    "P4CSICamera", cg.PollingComponent, cg.EntityBase
)

RESOLUTIONS = {
    "1928X1092": 0,
    "1280X720": 1,
    "640X480": 2,
}

CONFIG_SCHEMA = (
    cv.ENTITY_BASE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(P4CSICamera),
            cv.Optional(CONF_RESOLUTION, default="1928X1092"): cv.one_of(
                *RESOLUTIONS, upper=True
            ),
            cv.Optional(CONF_DATA_LANES, default=2): cv.int_range(min=1, max=2),
            cv.Optional(CONF_MAX_FRAMERATE, default="2 fps"): cv.All(
                cv.framerate, cv.Range(min=0, min_included=False, max=30)
            ),
            cv.Optional(CONF_IDLE_FRAMERATE, default="0.1 fps"): cv.All(
                cv.framerate, cv.Range(min=0, max=1)
            ),
            cv.Optional(CONF_RESET_PIN): cv.int_,
            cv.Optional(CONF_POWER_PIN): cv.int_,
            cv.Optional(CONF_I2C_ID): cv.use_id(i2c.I2CBus),
            cv.Optional(CONF_SCCB_FREQUENCY, default="100kHz"): cv.frequency,
            # Exposure and gain need i2c_id: they are written to the sensor over that bus.
            cv.Optional(CONF_AUTO_EXPOSURE, default=True): cv.boolean,
            cv.Optional(CONF_AE_TARGET, default=110): cv.int_range(min=16, max=240),
            cv.Optional(CONF_MAX_EXPOSURE, default="250ms"): cv.All(
                cv.positive_time_period_microseconds,
                cv.Range(min=cv.TimePeriod(milliseconds=1), max=cv.TimePeriod(milliseconds=1000)),
            ),
            cv.Optional(CONF_FLICKER_FREQUENCY, default="50Hz"): cv.Any(
                cv.one_of("off", lower=True), cv.All(cv.frequency, cv.one_of(50.0, 60.0))
            ),
            cv.Optional(CONF_JPEG_QUALITY, default=40): cv.int_range(min=10, max=100),
            cv.Optional(CONF_VERTICAL_FLIP, default=False): cv.boolean,
            cv.Optional(CONF_HORIZONTAL_FLIP, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    cg.add_define("USE_CAMERA")
    var = cg.new_Pvariable(config[CONF_ID])
    await setup_entity(var, config, "camera")
    await cg.register_component(var, config)

    cg.add(var.set_data_lanes(config[CONF_DATA_LANES]))
    cg.add(var.set_max_update_interval(int(1000 / config[CONF_MAX_FRAMERATE])))

    if config[CONF_IDLE_FRAMERATE] == 0:
        cg.add(var.set_idle_update_interval(0))
    else:
        cg.add(var.set_idle_update_interval(int(1000 / config[CONF_IDLE_FRAMERATE])))

    if CONF_RESET_PIN in config:
        cg.add(var.set_reset_pin(config[CONF_RESET_PIN]))
    if CONF_POWER_PIN in config:
        cg.add(var.set_power_pin(config[CONF_POWER_PIN]))
    if CONF_I2C_ID in config:
        bus = await cg.get_variable(config[CONF_I2C_ID])
        cg.add(var.set_i2c_bus(bus))
    cg.add(var.set_sccb_frequency(int(config[CONF_SCCB_FREQUENCY])))
    cg.add(var.set_auto_exposure(config[CONF_AUTO_EXPOSURE]))
    cg.add(var.set_ae_target(config[CONF_AE_TARGET]))
    cg.add(var.set_max_exposure_us(config[CONF_MAX_EXPOSURE].total_microseconds))
    flicker = config[CONF_FLICKER_FREQUENCY]
    cg.add(var.set_flicker_hz(0 if flicker == "off" else int(flicker)))

    cg.add(var.set_jpeg_quality(config[CONF_JPEG_QUALITY]))
    cg.add(var.set_vertical_flip(config[CONF_VERTICAL_FLIP]))
    cg.add(var.set_horizontal_flip(config[CONF_HORIZONTAL_FLIP]))

    cg.add_define("USE_P4_CSI_CAMERA")

    # ESP-IDF SDK config options needed for CSI camera
    for opt, val in {
        "CONFIG_CAMERA_OV02C10": True,
        "CONFIG_CAMERA_OV02C10_AUTO_DETECT": True,
        "CONFIG_CAMERA_OV02C10_AUTO_DETECT_MIPI_INTERFACE_SENSOR": True,
        "CONFIG_MIPI_CSI_ISR_IRAM_SAFE": False,
        "CONFIG_JPEG_ENABLE_ENCODE_ENGINE": True,
        "CONFIG_VFS_SUPPORT_IO": True,
        "CONFIG_VFS_SUPPORT_DIR": True,
        "CONFIG_VFS_SUPPORT_SELECT": True,
        "CONFIG_VFS_SUPPORT_TERMIOS": True,
        "CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE": True,
        "CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE": True,
        "CONFIG_ESP_VIDEO_ENABLE_ISP": True,
        "CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER": True,
        "CONFIG_ESP_VIDEO_DISABLE_MIPI_CSI_DRIVER_BACKUP_BUFFER": True,
        "CONFIG_ESP_IPA_ENABLED": True,
        "CONFIG_ESP_TASK_WDT_TIMEOUT_S": 120,
        "CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0": False,
        "CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1": False,
    }.items():
        add_idf_sdkconfig_option(opt, val)

    # Use official esp_video from registry + local OV02C10 driver
    add_idf_component(
        name="esp_video",
        repo="https://github.com/espressif/esp-video-components.git",
        path="esp_video",
        ref=ESP_VIDEO_REF,
    )
    add_idf_component(
        name="esp_cam_sensor",
        repo="https://github.com/espressif/esp-video-components.git",
        path="esp_cam_sensor",
        ref=ESP_VIDEO_REF,
    )
    add_idf_component(
        name="esp_ipa",
        repo="https://github.com/espressif/esp-video-components.git",
        path="esp_ipa",
        ref=ESP_VIDEO_REF,
    )
