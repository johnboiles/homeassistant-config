"""Local USB HID UPS prototype for ESP32-S3/P4, based on esp-cyberpower-ups."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.esp32 import add_idf_sdkconfig_option, only_on_variant
from esphome.const import CONF_ID

DEPENDENCIES = ["esp32"]
ns = cg.esphome_ns.namespace("usb_hid_ups")
UsbHidUpsComponent = ns.class_("UsbHidUpsComponent", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema({cv.GenerateID(): cv.declare_id(UsbHidUpsComponent)}).extend(cv.COMPONENT_SCHEMA),
    only_on_variant(supported=["ESP32S3", "ESP32P4"]),
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE", 1024)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_HUBS_SUPPORTED", False)
