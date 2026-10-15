#pragma once

#ifdef USE_ESP32_VARIANT_ESP32P4

#include <cmath>
#include "esphome/core/component.h"
#include "esphome/components/number/number.h"
#include "p4_csi_camera.h"

namespace esphome {
namespace p4_csi_camera {

enum NumberKind : uint8_t {
  NUMBER_JPEG_QUALITY = 0,
  NUMBER_EXPOSURE,
  NUMBER_ANALOG_GAIN,
  NUMBER_DIGITAL_GAIN,
  NUMBER_AE_TARGET,
};

class P4CSICameraNumber : public number::Number, public Component {
 public:
  void set_camera(P4CSICamera *camera) { this->camera_ = camera; }
  void set_kind(NumberKind kind) { this->kind_ = kind; }

  void setup() override {
    // Auto exposure moves exposure and gain by itself: follow it, so the entities show what the sensor runs at.
    this->camera_->add_on_exposure_callback([this]() { this->publish_current_(); });
    this->publish_current_();
  }

  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

 protected:
  float current_() const {
    switch (this->kind_) {
      case NUMBER_EXPOSURE:
        return this->camera_->get_exposure_ms();
      case NUMBER_ANALOG_GAIN:
        return this->camera_->get_analog_gain();
      case NUMBER_DIGITAL_GAIN:
        return this->camera_->get_digital_gain();
      case NUMBER_AE_TARGET:
        return this->camera_->get_ae_target();
      default:
        return this->camera_->get_jpeg_quality();
    }
  }

  void publish_current_() {
    bool sensor_value = this->kind_ == NUMBER_EXPOSURE || this->kind_ == NUMBER_ANALOG_GAIN ||
                        this->kind_ == NUMBER_DIGITAL_GAIN;
    if (sensor_value && !this->camera_->has_exposure_control())
      return;  // not read from the sensor yet (or no i2c_id): leave the entity unknown
    float value = this->current_();
    if (!this->has_state() || std::abs(value - this->state) > 0.001f)
      this->publish_state(value);
  }

  void control(float value) override {
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
      case NUMBER_AE_TARGET:
        this->camera_->set_ae_target_runtime((uint8_t) value);
        break;
      default:
        this->camera_->set_jpeg_quality_runtime((uint8_t) value);
        break;
    }
    this->publish_state(this->current_());
  }

  P4CSICamera *camera_{nullptr};
  NumberKind kind_{NUMBER_JPEG_QUALITY};
};

}  // namespace p4_csi_camera
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4
