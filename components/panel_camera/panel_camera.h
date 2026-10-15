#pragma once
#include "esphome/core/component.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::panel_camera {

// The plugin: tells Tessera the screen carries panel_camera. The camera is p4_csi_camera's; nothing here draws.
class PanelCamera : public Component, public tessera::Plugin {
 public:
  float get_setup_priority() const override { return setup_priority::LATE; }
};

}  // namespace esphome::panel_camera
