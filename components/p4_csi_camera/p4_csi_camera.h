#pragma once

#ifdef USE_ESP32_VARIANT_ESP32P4

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/camera/camera.h"
#include "esphome/components/i2c/i2c_bus.h"
#include "esphome/core/helpers.h"

#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "driver/jpeg_encode.h"

namespace esphome {
namespace p4_csi_camera {

using namespace esphome::camera;

class P4CSICamera;

// Frame buffer wrapper
struct P4CameraFrameBuffer {
  uint8_t *data{nullptr};
  size_t length{0};
  size_t width{0};
  size_t height{0};
};

// CameraImage implementation
class P4CSICameraImage : public camera::CameraImage {
 public:
  P4CSICameraImage(P4CameraFrameBuffer *buffer, uint8_t requesters);
  uint8_t *get_data_buffer() override;
  size_t get_data_length() override;
  bool was_requested_by(camera::CameraRequester requester) const override;
 protected:
  P4CameraFrameBuffer *buffer_;
  uint8_t requesters_;
};

// CameraImageReader implementation
class P4CSICameraImageReader : public camera::CameraImageReader {
 public:
  P4CSICameraImageReader() {}
  ~P4CSICameraImageReader() {}
  void set_image(std::shared_ptr<camera::CameraImage> image) override;
  size_t available() const override;
  uint8_t *peek_data_buffer() override;
  void consume_data(size_t consumed) override;
  void return_image() override;
 protected:
  std::shared_ptr<P4CSICameraImage> image_;
  size_t offset_{0};
};

// Main Camera Component
class P4CSICamera : public camera::Camera {
 public:
  P4CSICamera();

  void set_data_lanes(uint8_t lanes) { this->data_lanes_ = lanes; }
  void set_max_update_interval(uint32_t interval) { this->max_update_interval_ = interval; }
  void set_idle_update_interval(uint32_t interval) { this->idle_update_interval_ = interval; }
  void set_reset_pin(int8_t pin) { this->reset_pin_ = pin; }
  void set_power_pin(int8_t pin) { this->power_pin_ = pin; }
  void set_i2c_bus(i2c::InternalI2CBus *bus) { this->i2c_bus_ = bus; }
  void set_sccb_frequency(uint32_t frequency) { this->sccb_frequency_ = frequency; }
  void set_jpeg_quality(uint8_t quality) { this->jpeg_quality_ = quality; }
  void set_vertical_flip(bool flip) { this->vertical_flip_ = flip; }
  void set_horizontal_flip(bool flip) { this->horizontal_flip_ = flip; }

  // Runtime setters (apply immediately via V4L2 if initialized)
  void set_jpeg_quality_runtime(uint8_t quality);
  void set_vertical_flip_runtime(bool flip);
  void set_horizontal_flip_runtime(bool flip);

  // Exposure and gain, written straight to the OV02C10 over the shared I2C bus (needs i2c_id). The sensor has no
  // auto exposure of its own; with auto exposure on, loop() steers these towards ae_target from each frame's brightness.
  void set_auto_exposure(bool on) { this->auto_exposure_ = on; }
  void set_ae_target(uint8_t target) { this->ae_target_ = target; }
  void set_max_exposure_us(uint32_t us) { this->max_exposure_us_ = us; }
  void set_flicker_hz(uint8_t hz) { this->flicker_hz_ = hz; }

  void set_auto_exposure_runtime(bool on);
  void set_ae_target_runtime(uint8_t target);
  void set_exposure_ms_runtime(float ms);       // turns auto exposure off
  void set_analog_gain_runtime(float gain);     // turns auto exposure off
  void set_digital_gain_runtime(float gain);    // turns auto exposure off

  bool get_auto_exposure() const { return this->auto_exposure_; }
  uint8_t get_ae_target() const { return this->ae_target_; }
  float get_exposure_ms() const;
  float get_analog_gain() const { return this->analog_gain_ / 16.0f; }
  float get_digital_gain() const { return this->digital_gain_ / 1024.0f; }
  bool has_exposure_control() const { return this->exposure_ready_; }
  // Called when exposure, gain or auto exposure changed, at most once a second, so the entities show the values.
  void add_on_exposure_callback(std::function<void()> &&callback) { this->exposure_callback_.add(std::move(callback)); }

  uint8_t get_jpeg_quality() const { return this->jpeg_quality_; }
  bool get_vertical_flip() const { return this->vertical_flip_; }
  bool get_horizontal_flip() const { return this->horizontal_flip_; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void start_stream(camera::CameraRequester requester) override;
  void stop_stream(camera::CameraRequester requester) override;
  void request_image(camera::CameraRequester requester) override;
  void add_listener(camera::CameraListener *listener) override;
  void add_image_callback(std::function<void(std::shared_ptr<camera::CameraImage>)> &&callback);
  camera::CameraImageReader *create_image_reader() override;

 protected:
  bool has_requested_image_() const;
  bool read_reg_(uint16_t reg, uint8_t *value);
  bool write_reg_(uint16_t reg, uint8_t value);
  void read_exposure_();
  void apply_exposure_();
  void auto_expose_(const uint16_t *pixels, size_t num_pixels);
  void exposure_changed_();
  void fail_(const char *format, ...) __attribute__((format(printf, 2, 3)));

  // Configuration
  uint8_t data_lanes_{2};
  int8_t reset_pin_{-1};
  int8_t power_pin_{-1};
  i2c::InternalI2CBus *i2c_bus_{nullptr};
  uint32_t sccb_frequency_{100000};
  uint32_t max_update_interval_{500};
  uint32_t idle_update_interval_{10000};
  uint8_t jpeg_quality_{40};
  bool vertical_flip_{false};
  bool horizontal_flip_{false};

  // V4L2 video device
  int video_fd_{-1};
  uint8_t *frame_buffers_[6]{};
  size_t frame_buf_size_{0};
  uint16_t frame_width_{0};
  uint16_t frame_height_{0};

  // JPEG encoder
  jpeg_encoder_handle_t jpeg_handle_{nullptr};
  uint8_t *jpeg_out_buf_{nullptr};
  size_t jpeg_out_buf_size_{0};
  uint8_t *encode_src_buf_{nullptr};
  P4CameraFrameBuffer jpeg_buffer_;

  // State
  bool initialized_{false};
  char failure_[96]{};

  // Exposure state, in the sensor's units: lines, analog gain x16 (16 = 1x), digital gain x1024 (1024 = 1x).
  bool auto_exposure_{true};
  uint8_t ae_target_{110};
  uint32_t max_exposure_us_{250000};
  uint8_t flicker_hz_{50};
  bool exposure_ready_{false};
  uint32_t line_ns_{27918};
  uint16_t vts_default_{1164};
  uint16_t vts_{0};
  uint32_t exposure_lines_{0};
  uint16_t analog_gain_{16};
  uint32_t digital_gain_{1024};
  uint8_t last_brightness_{0};
  bool exposure_dirty_{false};
  uint32_t last_exposure_publish_{0};
  CallbackManager<void()> exposure_callback_{};  std::shared_ptr<P4CSICameraImage> current_image_;
  uint8_t single_requesters_{0};
  uint8_t stream_requesters_{0};
  uint64_t last_update_{0};

  // Callbacks
  CallbackManager<void(std::shared_ptr<camera::CameraImage>)> new_image_callback_{};
  std::vector<camera::CameraListener *> listeners_;
};

extern P4CSICamera *global_p4_csi_camera;

}  // namespace p4_csi_camera
}  // namespace esphome

// Include platform headers so codegen can find the classes
#include "p4_csi_camera_number.h"
#include "p4_csi_camera_switch.h"

#endif  // USE_ESP32_VARIANT_ESP32P4
