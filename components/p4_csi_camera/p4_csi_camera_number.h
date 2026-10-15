#pragma once

#ifdef USE_ESP32_VARIANT_ESP32P4

#include <cmath>
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/number/number.h"
#include "p4_csi_camera.h"

namespace esphome {
namespace p4_csi_camera {

enum NumberKind : uint8_t {
  NUMBER_JPEG_QUALITY = 0,
  NUMBER_EXPOSURE,
  NUMBER_ANALOG_GAIN,
  NUMBER_DIGITAL_GAIN,
};

class P4CSICameraNumber : public number::Number, public Component {
 public:
  void set_camera(P4CSICamera *camera) { this->camera_ = camera; }
  void set_kind(NumberKind kind) { this->kind_ = kind; }
  void set_restore_value(bool restore) { this->restore_value_ = restore; }

  // After the camera (DATA): it has read the sensor's exposure and gains by then.
  void setup() override {
    if (this->restore_value_) {
      this->pref_ = global_preferences->make_preference<float>(this->get_preference_hash());
      float saved;
      if (this->pref_.load(&saved) && std::isfinite(saved) && this->usable_()) {
        this->apply_(saved);
      }
    }
    if (this->usable_())
      this->publish_state(this->current_());
  }

  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

 protected:
  // Exposure and the gains need the sensor's registers (i2c_id); without them the entity stays unknown.
  bool usable_() const { return this->kind_ == NUMBER_JPEG_QUALITY || this->camera_->has_exposure_control(); }

  float current_() const {
    switch (this->kind_) {
      case NUMBER_EXPOSURE:
        return this->camera_->get_exposure_ms();
      case NUMBER_ANALOG_GAIN:
        return this->camera_->get_analog_gain();
      case NUMBER_DIGITAL_GAIN:
        return this->camera_->get_digital_gain();
      default:
        return this->camera_->get_jpeg_quality();
    }
  }

  void apply_(float value) {
    switch (this->kind_) {
      case NUMBER_EXPOSURE:
        this->camera_->set_exposure_ms_runtime(value);
        break;
      case NUMBER_ANALOG_GAIN:
        this->camera_->set_analog_gain_runtime(value);
        break;
      case NUMBER_DIGITAL_GAIN:
        this->camera_->set_digital_gain_runtime(value);
        break;
      default:
        this->camera_->set_jpeg_quality_runtime((uint8_t) value);
        break;
    }
  }

  void control(float value) override {
    if (!this->usable_())
      return;
    this->apply_(value);
    if (this->restore_value_)
      this->pref_.save(&value);
    this->publish_state(this->current_());
  }

  P4CSICamera *camera_{nullptr};
  NumberKind kind_{NUMBER_JPEG_QUALITY};
  bool restore_value_{true};
  ESPPreferenceObject pref_;
};

}  // namespace p4_csi_camera
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4
