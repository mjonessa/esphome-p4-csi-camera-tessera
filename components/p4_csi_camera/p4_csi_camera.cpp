#include "p4_csi_camera.h"

#ifdef USE_ESP32_VARIANT_ESP32P4

#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_cam_sensor_detect.h"
#include "ov02c10.h"
#include "driver/i2c_master.h"
#include "esp_task_wdt.h"
#include "linux/videodev2.h"
#include "driver/jpeg_encode.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <cstdarg>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <unistd.h>

namespace esphome {
namespace p4_csi_camera {

static const char *const TAG = "p4_csi_camera";

static constexpr int NUM_FRAME_BUFFERS = 4;
static constexpr uint16_t SENSOR_WIDTH = 1928;
static constexpr uint16_t SENSOR_HEIGHT = 1092;

P4CSICamera *global_p4_csi_camera = nullptr;  // NOLINT

// ===========================================================================
// P4CSICameraImage
// ===========================================================================

P4CSICameraImage::P4CSICameraImage(P4CameraFrameBuffer *buffer, uint8_t requesters)
    : buffer_(buffer), requesters_(requesters) {}

uint8_t *P4CSICameraImage::get_data_buffer() { return this->buffer_->data; }
size_t P4CSICameraImage::get_data_length() { return this->buffer_->length; }
bool P4CSICameraImage::was_requested_by(camera::CameraRequester requester) const {
  return this->requesters_ & (1 << (uint8_t)requester);
}

// ===========================================================================
// P4CSICameraImageReader
// ===========================================================================

void P4CSICameraImageReader::set_image(std::shared_ptr<camera::CameraImage> image) {
  this->image_ = std::static_pointer_cast<P4CSICameraImage>(image);
  this->offset_ = 0;
}
size_t P4CSICameraImageReader::available() const {
  if (!this->image_) return 0;
  return this->image_->get_data_length() - this->offset_;
}
uint8_t *P4CSICameraImageReader::peek_data_buffer() {
  if (!this->image_) return nullptr;
  return this->image_->get_data_buffer() + this->offset_;
}
void P4CSICameraImageReader::consume_data(size_t consumed) { this->offset_ += consumed; }
void P4CSICameraImageReader::return_image() {
  this->image_.reset();
  this->offset_ = 0;
}

// ===========================================================================
// P4CSICamera
// ===========================================================================

// ESPHome links its sources from an archive, so ov02c10_sensor.c, which nothing calls by name, was left out of the
// firmware and esp_video found no sensor driver to try. Naming its detect function here pulls the file in (as
// esp_cam_sensor does for its own drivers with "-u <sensor>_detect"); its linker fragment keeps the table entry.
static void *volatile keep_ov02c10_detect = reinterpret_cast<void *>(&ov02c10_detect);

P4CSICamera::P4CSICamera() { global_p4_csi_camera = this; }

// Logs why setup stopped and keeps it as the component's error, so the "marked FAILED" line of a log that connects
// after boot (over Wi-Fi, where the setup lines are long gone) still says what went wrong.
void P4CSICamera::fail_(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(this->failure_, sizeof(this->failure_), format, args);
  va_end(args);
  ESP_LOGE(TAG, "%s", this->failure_);
  this->mark_failed(LOG_STR(this->failure_));
}

void P4CSICamera::setup() {
  ESP_LOGI(TAG, "Setting up ESP32-P4 CSI Camera (OV02C10) via esp_video...");
  ESP_LOGI(TAG, "  Data lanes: %d", this->data_lanes_);
  ESP_LOGI(TAG, "  Max update interval: %d ms", this->max_update_interval_);

  // Step 1: Initialize the video subsystem (handles CSI + ISP + sensor detection)
  esp_video_init_csi_config_t csi_config = {};
  if (this->i2c_bus_ != nullptr) {
    // Share the ESPHome bus (the touch panel's): a second controller on GPIO7/GPIO8 would fail or steal the pins.
    i2c_master_bus_handle_t bus_handle = nullptr;
    esp_err_t bus_err = i2c_master_get_bus_handle((i2c_port_num_t) this->i2c_bus_->get_port(), &bus_handle);
    if (bus_err != ESP_OK || bus_handle == nullptr) {
      this->fail_("I2C bus on port %d is not ready: %s", this->i2c_bus_->get_port(), esp_err_to_name(bus_err));
      return;
    }
    ESP_LOGI(TAG, "  SCCB: shared ESPHome I2C bus, port %d", this->i2c_bus_->get_port());
    csi_config.sccb_config.init_sccb = false;
    csi_config.sccb_config.i2c_handle = bus_handle;
  } else {
    csi_config.sccb_config.init_sccb = true;  // Let esp_video manage sensor I2C
    csi_config.sccb_config.i2c_config.port = 0;  // Port 0 - same as where sensor lives
    csi_config.sccb_config.i2c_config.scl_pin = (gpio_num_t)8;
    csi_config.sccb_config.i2c_config.sda_pin = (gpio_num_t)7;
  }
  csi_config.sccb_config.freq = this->sccb_frequency_;
  csi_config.reset_pin = (gpio_num_t)-1;  // No reset pin
  csi_config.pwdn_pin = (gpio_num_t)-1;   // No power down pin

  esp_video_init_config_t cam_config = {};
  cam_config.csi = &csi_config;

  esp_cam_sensor_detect_fn_t *detect_start = nullptr;
  esp_cam_sensor_detect_fn_t *detect_end = nullptr;
  esp_cam_sensor_detect_get_array(&detect_start, &detect_end);
  ESP_LOGI(TAG, "  Sensor drivers linked: %d (OV02C10 at %p)", (int) (detect_end - detect_start), keep_ov02c10_detect);

  esp_err_t err = esp_video_init(&cam_config);
  if (err != ESP_OK) {
    this->fail_("esp_video_init failed: %s", esp_err_to_name(err));
    return;
  }
  ESP_LOGI(TAG, "esp_video initialized successfully");

  // The MIPI-CSI device exists only when a sensor answered: esp_video_init returns ESP_OK without one. Any other
  // /dev/video* is the ISP or the JPEG encoder, which take no capture format (VIDIOC_G_FMT then fails with EINVAL).
  this->video_fd_ = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY | O_NONBLOCK, 0);
  if (this->video_fd_ < 0) {
    this->fail_("OV02C10 not detected on I2C 0x%02x (%d sensor drivers linked), %s missing", OV02C10_SCCB_ADDR,
                (int) (detect_end - detect_start), ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
    return;
  }
  ESP_LOGI(TAG, "Found video device at %s (fd=%d)", ESP_VIDEO_MIPI_CSI_DEVICE_NAME, this->video_fd_);

  // Step 3: Set the pixel format to RGB565
  struct v4l2_format fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(this->video_fd_, VIDIOC_G_FMT, &fmt) != 0) {
    this->fail_("VIDIOC_G_FMT failed (errno %d)", errno);
    return;
  }

  ESP_LOGI(TAG, "Default format: %dx%d, pixfmt=0x%08x",
           fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat);

  // Log the pixel format as a FourCC string for readability
  char fourcc[5] = {};
  fourcc[0] = (fmt.fmt.pix.pixelformat >> 0) & 0xFF;
  fourcc[1] = (fmt.fmt.pix.pixelformat >> 8) & 0xFF;
  fourcc[2] = (fmt.fmt.pix.pixelformat >> 16) & 0xFF;
  fourcc[3] = (fmt.fmt.pix.pixelformat >> 24) & 0xFF;
  ESP_LOGI(TAG, "Default pixfmt FourCC: '%s'", fourcc);

  // Request RGB565 output format
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
  if (ioctl(this->video_fd_, VIDIOC_S_FMT, &fmt) != 0) {
    ESP_LOGW(TAG, "VIDIOC_S_FMT RGB565 failed, using default format");
  }

  // Re-read format to see what we actually got
  if (ioctl(this->video_fd_, VIDIOC_G_FMT, &fmt) == 0) {
    fourcc[0] = (fmt.fmt.pix.pixelformat >> 0) & 0xFF;
    fourcc[1] = (fmt.fmt.pix.pixelformat >> 8) & 0xFF;
    fourcc[2] = (fmt.fmt.pix.pixelformat >> 16) & 0xFF;
    fourcc[3] = (fmt.fmt.pix.pixelformat >> 24) & 0xFF;
    ESP_LOGI(TAG, "Actual format after S_FMT: %dx%d, pixfmt='%s' (0x%08x)",
             fmt.fmt.pix.width, fmt.fmt.pix.height, fourcc, fmt.fmt.pix.pixelformat);
  }

  this->frame_width_ = fmt.fmt.pix.width;
  this->frame_height_ = fmt.fmt.pix.height;
  ESP_LOGI(TAG, "Using format: %dx%d", this->frame_width_, this->frame_height_);

  // Step 4: Request frame buffers
  struct v4l2_requestbuffers req = {};
  req.count = NUM_FRAME_BUFFERS;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (ioctl(this->video_fd_, VIDIOC_REQBUFS, &req) != 0) {
    this->fail_("VIDIOC_REQBUFS failed (errno %d)", errno);
    return;
  }

  // Step 5: Map frame buffers
  for (int i = 0; i < NUM_FRAME_BUFFERS; i++) {
    struct v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;

    if (ioctl(this->video_fd_, VIDIOC_QUERYBUF, &buf) != 0) {
      this->fail_("VIDIOC_QUERYBUF failed for buf %d (errno %d)", i, errno);
      return;
    }

    this->frame_buffers_[i] = (uint8_t *)mmap(NULL, buf.length,
        PROT_READ | PROT_WRITE, MAP_SHARED, this->video_fd_, buf.m.offset);
    if (this->frame_buffers_[i] == nullptr || this->frame_buffers_[i] == MAP_FAILED) {
      this->frame_buffers_[i] = nullptr;
      this->fail_("mmap failed for buf %d", i);
      return;
    }
    this->frame_buf_size_ = buf.length;

    // Queue the buffer
    if (ioctl(this->video_fd_, VIDIOC_QBUF, &buf) != 0) {
      this->fail_("VIDIOC_QBUF failed for buf %d (errno %d)", i, errno);
      return;
    }
  }

  ESP_LOGI(TAG, "Mapped %d frame buffers (%d bytes each)", NUM_FRAME_BUFFERS, this->frame_buf_size_);

  // Step 6: Start streaming
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(this->video_fd_, VIDIOC_STREAMON, &type) != 0) {
    this->fail_("VIDIOC_STREAMON failed (errno %d)", errno);
    return;
  }
  ESP_LOGI(TAG, "Video streaming started!");

  // Note: Flip/mirror is applied in software during the loop() frame processing.
  // V4L2_CID_VFLIP/HFLIP are not supported by this sensor driver.

  // Note: Auto White Balance is handled by the esp_ipa (Image Processing Algorithm)
  // component via CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER.
  // No manual V4L2 AWB control needed.

  // Step 7: Init JPEG encoder
  jpeg_encode_engine_cfg_t jpeg_cfg = {};
  jpeg_cfg.timeout_ms = 1000;
  err = jpeg_new_encoder_engine(&jpeg_cfg, &this->jpeg_handle_);
  if (err != ESP_OK) {
    this->fail_("JPEG encoder init failed: %s", esp_err_to_name(err));
    return;
  }

  // Allocate JPEG output buffer using the JPEG encoder's aligned allocator
  this->jpeg_out_buf_size_ = this->frame_buf_size_;
  jpeg_encode_memory_alloc_cfg_t mem_cfg = {};
  mem_cfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  this->jpeg_out_buf_ = (uint8_t *)jpeg_alloc_encoder_mem(this->jpeg_out_buf_size_, &mem_cfg, &this->jpeg_out_buf_size_);
  if (!this->jpeg_out_buf_) {
    this->fail_("Failed to alloc JPEG output buffer");
    return;
  }

  this->jpeg_buffer_.data = this->jpeg_out_buf_;
  this->jpeg_buffer_.width = this->frame_width_;
  this->jpeg_buffer_.height = this->frame_height_;

  this->read_exposure_();

  this->initialized_ = true;
  ESP_LOGI(TAG, "Camera initialized successfully! Streaming %dx%d", this->frame_width_, this->frame_height_);
}

void P4CSICamera::loop() {
  if (!this->initialized_) return;

  // Feed watchdog every loop iteration to prevent WDT reset during idle
  esp_task_wdt_reset();

  uint32_t now = millis();
  if (this->exposure_dirty_ && now - this->last_exposure_publish_ >= 1000) {
    this->exposure_dirty_ = false;
    this->last_exposure_publish_ = now;
    this->exposure_callback_.call();
  }
  bool needs_image = this->has_requested_image_();
  bool streaming = this->stream_requesters_ != 0;
  uint32_t interval = streaming ? this->max_update_interval_ : this->idle_update_interval_;

  if (now - this->last_update_ < interval) return;

  // Take the newest frame and hand the older ones straight back. The driver fills the queued buffers in order and then
  // waits, so the first one out can be many seconds old after an idle spell, and auto exposure would steer on it.
  struct v4l2_buffer buf = {};
  bool have_frame = false;
  for (int i = 0; i < NUM_FRAME_BUFFERS; i++) {
    struct v4l2_buffer next = {};
    next.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    next.memory = V4L2_MEMORY_MMAP;
    if (ioctl(this->video_fd_, VIDIOC_DQBUF, &next) != 0)
      break;
    if (have_frame && ioctl(this->video_fd_, VIDIOC_QBUF, &buf) != 0)
      ESP_LOGW(TAG, "VIDIOC_QBUF failed (errno %d)", errno);
    buf = next;
    have_frame = true;
  }
  if (!have_frame) {
    if (errno != EAGAIN) {
      ESP_LOGW(TAG, "VIDIOC_DQBUF failed: errno=%d", errno);
    }
    return;
  }

  uint8_t *frame_data = this->frame_buffers_[buf.index];
  size_t frame_size = buf.bytesused > 0 ? buf.bytesused : this->frame_buf_size_;

  // Apply software white balance correction to RGB565 data
  // The ISP demosaic produces green-biased output without AWB.
  // RGB565 layout: RRRR RGGG GGGB BBBB (big-endian: high byte first)
  // On little-endian (ESP32-P4): byte[0] = GGGB BBBB, byte[1] = RRRR RGGG
  {
    uint16_t *pixels = (uint16_t *)frame_data;
    size_t num_pixels = frame_size / 2;

    // White balance correction
    for (size_t i = 0; i < num_pixels; i++) {
      uint16_t px = pixels[i];
      uint32_t r = (px >> 11) & 0x1F;  // 5 bits
      uint32_t g = (px >> 5) & 0x3F;   // 6 bits
      uint32_t b = px & 0x1F;           // 5 bits

      // Boost red and blue to compensate for green Bayer dominance
      // Gains: R=1.3, G=0.9, B=1.3 (gentle correction preserving brightness)
      r = (r * 166) >> 7;  // * 1.30
      g = (g * 115) >> 7;  // * 0.90
      b = (b * 166) >> 7;  // * 1.30

      // Clamp
      if (r > 31) r = 31;
      if (g > 63) g = 63;
      if (b > 31) b = 31;

      pixels[i] = (r << 11) | (g << 5) | b;
    }

    this->auto_expose_(pixels, num_pixels);

    // Software vertical flip (swap rows top-to-bottom)
    if (this->vertical_flip_) {
      uint16_t *row_buf = (uint16_t *)alloca(this->frame_width_ * 2);
      for (int y = 0; y < this->frame_height_ / 2; y++) {
        uint16_t *top = pixels + y * this->frame_width_;
        uint16_t *bot = pixels + (this->frame_height_ - 1 - y) * this->frame_width_;
        memcpy(row_buf, top, this->frame_width_ * 2);
        memcpy(top, bot, this->frame_width_ * 2);
        memcpy(bot, row_buf, this->frame_width_ * 2);
      }
    }

    // Software horizontal flip (mirror each row)
    if (this->horizontal_flip_) {
      for (int y = 0; y < this->frame_height_; y++) {
        uint16_t *row = pixels + y * this->frame_width_;
        for (int x = 0; x < this->frame_width_ / 2; x++) {
          uint16_t tmp = row[x];
          row[x] = row[this->frame_width_ - 1 - x];
          row[this->frame_width_ - 1 - x] = tmp;
        }
      }
    }
  }

  // Feed the watchdog before heavy encode operation
  esp_task_wdt_reset();

  // Encode directly from the V4L2 buffer (hardware JPEG encoder uses DMA, fast)
  jpeg_encode_cfg_t enc_cfg = {};
  enc_cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
  enc_cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
  enc_cfg.image_quality = this->jpeg_quality_;
  enc_cfg.width = this->frame_width_;
  enc_cfg.height = this->frame_height_;

  uint32_t jpeg_size = 0;
  esp_err_t err = jpeg_encoder_process(this->jpeg_handle_, &enc_cfg,
                                       frame_data, frame_size,
                                       this->jpeg_out_buf_, this->jpeg_out_buf_size_,
                                       &jpeg_size);

  // Return the buffer after encoding
  if (ioctl(this->video_fd_, VIDIOC_QBUF, &buf) != 0) {
    ESP_LOGW(TAG, "VIDIOC_QBUF failed");
  }

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "JPEG encode failed: %s", esp_err_to_name(err));
    return;
  }

  this->jpeg_buffer_.length = (size_t)jpeg_size;
  this->last_update_ = now;

  ESP_LOGD(TAG, "Frame: %dx%d, JPEG: %d bytes", this->frame_width_, this->frame_height_, jpeg_size);

  if (needs_image || streaming) {
    uint8_t requesters = this->single_requesters_ | this->stream_requesters_;
    this->single_requesters_ = 0;

    auto image = std::make_shared<P4CSICameraImage>(&this->jpeg_buffer_, requesters);
    this->current_image_ = image;
    this->new_image_callback_.call(image);
    for (auto *listener : this->listeners_) {
      listener->on_camera_image(image);
    }
    this->current_image_.reset();
  }
}

void P4CSICamera::dump_config() {
  ESP_LOGCONFIG(TAG, "ESP32-P4 CSI Camera (OV02C10):");
  ESP_LOGCONFIG(TAG, "  Resolution: %dx%d", this->frame_width_, this->frame_height_);
  ESP_LOGCONFIG(TAG, "  Data Lanes: %d", this->data_lanes_);
  ESP_LOGCONFIG(TAG, "  Max Update Interval: %d ms", this->max_update_interval_);
  ESP_LOGCONFIG(TAG, "  Initialized: %s", this->initialized_ ? "YES" : "NO");
  if (this->exposure_ready_) {
    ESP_LOGCONFIG(TAG, "  Auto exposure: %s (target %u, max %.0f ms, flicker %u Hz)", this->auto_exposure_ ? "ON" : "OFF",
                  this->ae_target_, this->max_exposure_us_ / 1000.0f, this->flicker_hz_);
    ESP_LOGCONFIG(TAG, "  Exposure: %.1f ms, analog gain %.2fx, digital gain %.2fx", this->get_exposure_ms(),
                  this->get_analog_gain(), this->get_digital_gain());
  } else {
    ESP_LOGCONFIG(TAG, "  Exposure control: unavailable (needs i2c_id)");
  }
  if (this->failure_[0] != '\0')
    ESP_LOGCONFIG(TAG, "  Failed: %s", this->failure_);
}

// Camera interface
void P4CSICamera::start_stream(camera::CameraRequester requester) {
  this->stream_requesters_ |= (1 << (uint8_t)requester);
}
void P4CSICamera::stop_stream(camera::CameraRequester requester) {
  this->stream_requesters_ &= ~(1 << (uint8_t)requester);
}
void P4CSICamera::request_image(camera::CameraRequester requester) {
  this->single_requesters_ |= (1 << (uint8_t)requester);
}
void P4CSICamera::add_listener(camera::CameraListener *listener) {
  this->listeners_.push_back(listener);
}
void P4CSICamera::add_image_callback(
    std::function<void(std::shared_ptr<camera::CameraImage>)> &&callback) {
  this->new_image_callback_.add(std::move(callback));
}
camera::CameraImageReader *P4CSICamera::create_image_reader() {
  return new P4CSICameraImageReader();
}
bool P4CSICamera::has_requested_image_() const {
  return this->single_requesters_ != 0;
}

// ---------------------------------------------------------------------------
// Exposure and gain
// ---------------------------------------------------------------------------
// OV02C10 registers (as Linux's ov02c10 driver uses them): exposure in lines at 0x3501-0x3502, analog gain at
// 0x3508-0x3509 as gain x16 shifted left 4 (0x10 = 1x, 0xf8 = 15.5x), digital gain at 0x350a-0x350c as gain x1024
// shifted left 6 (0x400 = 1x), frame length (VTS) at 0x380e-0x380f, line length (HTS) at 0x380c-0x380d. Exposure stays
// at least 8 lines under VTS, so a longer exposure stretches the frame (fewer sensor frames a second, fine at 2 fps).
static constexpr uint32_t OV02C10_PCLK_HZ = 81666700;
static constexpr uint16_t OV02C10_EXPOSURE_MIN = 4;
static constexpr uint16_t OV02C10_EXPOSURE_MARGIN = 8;
static constexpr uint16_t OV02C10_AGAIN_MIN = 0x10;
static constexpr uint16_t OV02C10_AGAIN_MAX = 0xf8;
static constexpr uint32_t OV02C10_DGAIN_MIN = 1024;
static constexpr uint32_t OV02C10_DGAIN_MAX = 4096;  // 4x: past this the picture is mostly noise
static constexpr uint32_t OV02C10_VTS_MAX = 0xffff;

bool P4CSICamera::read_reg_(uint16_t reg, uint8_t *value) {
  uint8_t addr[2] = {(uint8_t) (reg >> 8), (uint8_t) (reg & 0xff)};
  return this->i2c_bus_->write_readv(OV02C10_SCCB_ADDR, addr, 2, value, 1) == i2c::ERROR_OK;
}

bool P4CSICamera::write_reg_(uint16_t reg, uint8_t value) {
  uint8_t data[3] = {(uint8_t) (reg >> 8), (uint8_t) (reg & 0xff), value};
  return this->i2c_bus_->write_readv(OV02C10_SCCB_ADDR, data, 3, nullptr, 0) == i2c::ERROR_OK;
}

void P4CSICamera::read_exposure_() {
  if (this->i2c_bus_ == nullptr) {
    ESP_LOGW(TAG, "No i2c_id: exposure and gain controls are off");
    return;
  }
  uint8_t r[9];
  const uint16_t regs[9] = {0x3501, 0x3502, 0x3508, 0x3509, 0x350a, 0x350b, 0x350c, 0x380e, 0x380f};
  for (int i = 0; i < 9; i++) {
    if (!this->read_reg_(regs[i], &r[i])) {
      ESP_LOGW(TAG, "Reading sensor register 0x%04x failed: exposure and gain controls are off", regs[i]);
      return;
    }
  }
  uint8_t hts_hi, hts_lo;
  if (this->read_reg_(0x380c, &hts_hi) && this->read_reg_(0x380d, &hts_lo)) {
    uint32_t hts = (hts_hi << 8) | hts_lo;
    if (hts > 0)
      this->line_ns_ = (uint32_t) ((uint64_t) hts * 1000000000ULL / OV02C10_PCLK_HZ);
  }
  this->exposure_lines_ = (r[0] << 8) | r[1];
  this->analog_gain_ = (((r[2] << 8) | r[3]) >> 4);
  this->digital_gain_ = (((uint32_t) r[4] << 16) | (r[5] << 8) | r[6]) >> 6;
  this->vts_default_ = (r[7] << 8) | r[8];
  this->vts_ = this->vts_default_;
  this->analog_gain_ = std::clamp<uint16_t>(this->analog_gain_, OV02C10_AGAIN_MIN, OV02C10_AGAIN_MAX);
  this->digital_gain_ = std::clamp<uint32_t>(this->digital_gain_, OV02C10_DGAIN_MIN, OV02C10_DGAIN_MAX);
  this->exposure_ready_ = true;
  ESP_LOGI(TAG, "Sensor exposure: %u lines (%.1f ms, line %u ns, VTS %u), analog gain %.2fx, digital gain %.2fx",
           (unsigned) this->exposure_lines_, this->get_exposure_ms(), (unsigned) this->line_ns_, this->vts_default_,
           this->get_analog_gain(), this->get_digital_gain());
  this->exposure_dirty_ = true;
}

float P4CSICamera::get_exposure_ms() const { return this->exposure_lines_ * (this->line_ns_ / 1000000.0f); }

void P4CSICamera::apply_exposure_() {
  if (!this->exposure_ready_)
    return;
  uint32_t max_lines = OV02C10_VTS_MAX - OV02C10_EXPOSURE_MARGIN;
  this->exposure_lines_ = std::clamp<uint32_t>(this->exposure_lines_, OV02C10_EXPOSURE_MIN, max_lines);
  this->analog_gain_ = std::clamp<uint16_t>(this->analog_gain_, OV02C10_AGAIN_MIN, OV02C10_AGAIN_MAX);
  this->digital_gain_ = std::clamp<uint32_t>(this->digital_gain_, OV02C10_DGAIN_MIN, OV02C10_DGAIN_MAX);
  uint32_t vts = std::max<uint32_t>(this->vts_default_, this->exposure_lines_ + OV02C10_EXPOSURE_MARGIN);
  uint16_t again = this->analog_gain_ << 4;
  uint32_t dgain = this->digital_gain_ << 6;
  bool ok = true;
  if (vts != this->vts_) {
    ok &= this->write_reg_(0x380e, vts >> 8);
    ok &= this->write_reg_(0x380f, vts & 0xff);
  }
  ok &= this->write_reg_(0x3501, this->exposure_lines_ >> 8);
  ok &= this->write_reg_(0x3502, this->exposure_lines_ & 0xff);
  ok &= this->write_reg_(0x3508, again >> 8);
  ok &= this->write_reg_(0x3509, again & 0xff);
  ok &= this->write_reg_(0x350a, (dgain >> 16) & 0xff);
  ok &= this->write_reg_(0x350b, (dgain >> 8) & 0xff);
  ok &= this->write_reg_(0x350c, dgain & 0xff);
  if (!ok)
    ESP_LOGW(TAG, "Writing exposure to the sensor failed");
  this->vts_ = vts;
  this->exposure_changed_();
}

void P4CSICamera::exposure_changed_() { this->exposure_dirty_ = true; }

// One step of auto exposure from a frame: the mean brightness of a sample of its pixels against ae_target. Exposure
// goes up first (to max_exposure), then analog gain, then digital gain; down in the reverse order. Under mains light
// the exposure keeps to whole half-periods of the mains (10 ms at 50 Hz) once it is that long, so lights don't band.
void P4CSICamera::auto_expose_(const uint16_t *pixels, size_t num_pixels) {
  if (!this->exposure_ready_ || num_pixels == 0)
    return;
  uint32_t sum = 0, count = 0;
  for (size_t i = 0; i < num_pixels; i += 61) {
    uint16_t px = pixels[i];
    uint32_t r = ((px >> 11) & 0x1F) << 3, g = ((px >> 5) & 0x3F) << 2, b = (px & 0x1F) << 3;
    sum += (77 * r + 150 * g + 29 * b) >> 8;
    count++;
  }
  uint8_t mean = count ? sum / count : 0;
  this->last_brightness_ = mean;
  if (!this->auto_exposure_)
    return;

  float ratio = (float) this->ae_target_ / std::max<uint8_t>(mean, 1);
  if (ratio > 0.92f && ratio < 1.08f)
    return;
  // Big steps when far off (a dark room after a bright one), gentle ones near the target so it doesn't hunt.
  float limit = (ratio > 3.0f || ratio < 0.33f) ? 4.0f : 1.6f;
  ratio = std::clamp(ratio, 1.0f / limit, limit);

  float line_us = this->line_ns_ / 1000.0f;
  float total = this->exposure_lines_ * (this->analog_gain_ / 16.0f) * (this->digital_gain_ / 1024.0f) * ratio;
  float max_lines = std::max(this->max_exposure_us_ / line_us, (float) OV02C10_EXPOSURE_MIN);

  float lines = std::clamp(total, (float) OV02C10_EXPOSURE_MIN, max_lines);
  if (this->flicker_hz_ > 0) {
    float step = 1000000.0f / (2.0f * this->flicker_hz_) / line_us;
    if (lines >= step)
      lines = std::max(step, std::floor(lines / step) * step);
  }
  float rest = total / lines;
  float again = std::clamp(rest, 1.0f, OV02C10_AGAIN_MAX / 16.0f);
  rest /= again;
  float dgain = std::clamp(rest, 1.0f, OV02C10_DGAIN_MAX / 1024.0f);

  uint32_t new_lines = (uint32_t) lines;
  uint16_t new_again = (uint16_t) (again * 16.0f + 0.5f);
  uint32_t new_dgain = (uint32_t) (dgain * 1024.0f + 0.5f);
  if (new_lines == this->exposure_lines_ && new_again == this->analog_gain_ && new_dgain == this->digital_gain_)
    return;  // at a limit: as bright or as dark as it gets
  ESP_LOGD(TAG, "AE: brightness %u -> target %u: %.1f ms, gain %.2fx, digital %.2fx", mean, this->ae_target_,
           new_lines * line_us / 1000.0f, new_again / 16.0f, new_dgain / 1024.0f);
  this->exposure_lines_ = new_lines;
  this->analog_gain_ = new_again;
  this->digital_gain_ = new_dgain;
  this->apply_exposure_();
}

void P4CSICamera::set_auto_exposure_runtime(bool on) {
  this->auto_exposure_ = on;
  ESP_LOGI(TAG, "Auto exposure %s", on ? "ON" : "OFF");
  this->exposure_changed_();
}

void P4CSICamera::set_ae_target_runtime(uint8_t target) {
  this->ae_target_ = target;
  ESP_LOGI(TAG, "Auto exposure target set to %u", target);
  this->exposure_changed_();
}

void P4CSICamera::set_exposure_ms_runtime(float ms) {
  this->auto_exposure_ = false;
  this->exposure_lines_ = (uint32_t) (ms * 1000000.0f / this->line_ns_);
  ESP_LOGI(TAG, "Exposure set to %.1f ms (auto exposure off)", ms);
  this->apply_exposure_();
}

void P4CSICamera::set_analog_gain_runtime(float gain) {
  this->auto_exposure_ = false;
  this->analog_gain_ = (uint16_t) (gain * 16.0f + 0.5f);
  ESP_LOGI(TAG, "Analog gain set to %.2fx (auto exposure off)", gain);
  this->apply_exposure_();
}

void P4CSICamera::set_digital_gain_runtime(float gain) {
  this->auto_exposure_ = false;
  this->digital_gain_ = (uint32_t) (gain * 1024.0f + 0.5f);
  ESP_LOGI(TAG, "Digital gain set to %.2fx (auto exposure off)", gain);
  this->apply_exposure_();
}

void P4CSICamera::set_jpeg_quality_runtime(uint8_t quality) {
  this->jpeg_quality_ = quality;
  ESP_LOGI(TAG, "JPEG quality set to %d", quality);
}

void P4CSICamera::set_vertical_flip_runtime(bool flip) {
  this->vertical_flip_ = flip;
  ESP_LOGI(TAG, "Vertical flip set to %s", flip ? "ON" : "OFF");
}

void P4CSICamera::set_horizontal_flip_runtime(bool flip) {
  this->horizontal_flip_ = flip;
  ESP_LOGI(TAG, "Horizontal flip set to %s", flip ? "ON" : "OFF");
}

}  // namespace p4_csi_camera
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4
