#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/display/display.h"

namespace esphome {
namespace epaper_gallery {

enum ScaleMode { SCALE_FIT, SCALE_FILL };
enum ColorSpace { CS_RGB, CS_LAB };
enum DitherMethod { DITHER_NONE, DITHER_FLOYD_STEINBERG, DITHER_ATKINSON, DITHER_JJN };

/**
 * Loads JPEG/BMP files from a mounted filesystem (SD card), scales them to the
 * display, Floyd–Steinberg-dithers them to the 6-colour Spectra E6 palette and
 * pushes the result to the display buffer.
 *
 * Decoding, scaling and dithering run in a FreeRTOS task pinned to core 1 so
 * the ESPHome main loop (BLE proxy, API, web server) stays responsive. The
 * result is a palette-index buffer that the main loop copies into the display
 * and flushes with update().
 */
class EPaperGallery : public Component {
 public:
  void set_display(display::Display *d) { this->display_ = d; }
  void set_root_path(const std::string &p) { this->root_path_ = p; }
  void set_scale_mode(ScaleMode m) { this->scale_mode_ = m; }
  // Read at render start, so these can also be changed at runtime (e.g. from a
  // template number/select) and take effect with the next image.
  void set_dither_method(DitherMethod m) { this->dither_method_ = m; }
  /// Max accumulated error per channel added to a pixel (0..255, 255 = no clamp)
  void set_error_clamp(int c) { this->error_clamp_ = c; }
  /// Fraction of the quantisation error that gets diffused (0..1)
  void set_dither_strength(float s) { this->dither_strength_ = s; }
  void set_auto_rotate(bool r) { this->auto_rotate_ = r; }
  void set_color_space(ColorSpace cs) { this->color_space_ = cs; }
  /// Calibrated appearance of palette entry i (0 black, 1 white, 2 yellow, 3 red, 4 blue, 5 green)
  void set_match_color(int i, uint8_t r, uint8_t g, uint8_t b);
  /// Switch the matching palette at runtime: "config" (YAML palette / built-in default),
  /// "measured" (first calibration photo), "aitjcize" (esp32-photoframe), "waveshare" (pure RGB).
  /// Takes effect with the next render. Returns false for an unknown name.
  bool set_palette_preset(const std::string &name);
  /// Draw the six solid colour patches for calibration (call display update yourself afterwards)
  void draw_calibration_pattern();

  float get_setup_priority() const override { return setup_priority::LATE; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  /// Re-scan root_path for *.jpg/*.jpeg/*.bmp and fire list-changed callbacks.
  void refresh_list();
  const std::vector<std::string> &get_files() const { return this->files_; }
  std::string get_current() const;

  /// Schedule a file (name relative to root_path) for rendering.
  bool show(const std::string &filename);
  bool show_next();
  bool show_previous();
  bool is_busy() const { return this->task_running_.load(); }

  void add_on_list_changed_callback(std::function<void()> &&cb) { this->list_changed_cb_.add(std::move(cb)); }
  void add_on_image_shown_callback(std::function<void(std::string)> &&cb) {
    this->image_shown_cb_.add(std::move(cb));
  }

  /// Internal: JPEGDEC draw callback target (void* to avoid leaking JPEGDEC.h into the header)
  void jpeg_block(void *draw);
  /// Internal: body of the render task
  void render_task_body_();

 protected:
  bool render_(const std::string &path);
  bool decode_jpeg_(uint8_t *data, size_t size);
  bool decode_bmp_(uint8_t *data, size_t size);
  void resample_();
  void quantize_();
  void prepare_palette_();
  int nearest_(const int v[3]) const;
  void blit_and_update_();
  void free_buffers_();

  display::Display *display_{nullptr};
  std::string root_path_{"/sdcard/images"};
  ScaleMode scale_mode_{SCALE_FIT};
  DitherMethod dither_method_{DITHER_FLOYD_STEINBERG};
  int error_clamp_{64};
  float dither_strength_{1.0f};
  bool auto_rotate_{true};
  ColorSpace color_space_{CS_LAB};
  int16_t match_rgb_[6][3] = {{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1},
                              {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}};  // -1 = use built-in default
  int16_t config_rgb_[6][3];  // match_rgb_ after setup(), restored by preset "config"
  float match_lab_[6][3];     // same, in CIELAB (computed when rendering)

  std::vector<std::string> files_;
  int current_index_{-1};
  std::string pending_;

  // render task state
  std::string task_file_;
  std::atomic<bool> task_running_{false};
  std::atomic<bool> task_done_{false};
  bool task_ok_{false};

  // decoded source image, RGB888
  uint8_t *src_{nullptr};
  int src_w_{0}, src_h_{0};
  // display-sized working image, RGB888
  uint8_t *dst_{nullptr};
  int dst_w_{0}, dst_h_{0};
  // display-sized palette index buffer (one byte per pixel), produced by the task
  uint8_t *idx_{nullptr};

  CallbackManager<void()> list_changed_cb_;
  CallbackManager<void(std::string)> image_shown_cb_;
};

}  // namespace epaper_gallery
}  // namespace esphome
