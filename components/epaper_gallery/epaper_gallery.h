#pragma once

#include <atomic>
#include <cmath>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/display/display.h"

namespace esphome {
namespace epaper_gallery {

enum ScaleMode { SCALE_FIT, SCALE_FILL };
enum ColorSpace { CS_RGB, CS_LAB };
enum ToneCurve { TONE_LINEAR, TONE_SOFT };
enum DitherMethod { DITHER_NONE, DITHER_FLOYD_STEINBERG, DITHER_ATKINSON, DITHER_JJN, DITHER_DIZZY };

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
  // Rendering settings. Changing one at runtime (e.g. from a template number/select)
  // re-renders the current image after a short debounce. Per-image values are loaded
  // from "<image>.cfg" (or "_default.cfg") next to the image when it is shown.
  void set_dither_method(DitherMethod m) { this->update_(this->dither_method_, m); }
  /// Max accumulated error per channel added to a pixel (0..255, 255 = no clamp)
  void set_error_clamp(int c) { this->update_(this->error_clamp_, c); }
  /// Fraction of the quantisation error that gets diffused (0..1)
  void set_dither_strength(float s) { this->update_(this->dither_strength_, s); }
  /// Map the photo's 0..255 into the panel's black..white range (0 = off, 1 = full)
  void set_tone_compression(float k) { this->update_(this->tone_compression_, k); }
  void set_tone_curve(ToneCurve c) { this->update_(this->tone_curve_, c); }
  /// Colour saturation applied before dithering (1 = unchanged, 1.3 = +30 %)
  void set_saturation(float s) { this->update_(this->saturation_, s); }

  const std::string &get_palette_preset() const { return this->palette_preset_; }
  const char *get_dither_method_name() const;
  const char *get_tone_curve_name() const;
  int get_error_clamp() const { return this->error_clamp_; }
  float get_dither_strength() const { return this->dither_strength_; }
  float get_tone_compression() const { return this->tone_compression_; }
  float get_saturation() const { return this->saturation_; }

  /// Write the current settings to "<current image>.cfg" (for_image) or "_default.cfg"
  bool save_settings(bool for_image);
  /// Re-render the current image with the current settings (no settings file is loaded)
  bool rerender();
  /// Fired after settings were loaded from a file, so UI entities can be synced
  void add_on_settings_loaded_callback(std::function<void()> &&cb) { this->settings_loaded_cb_.add(std::move(cb)); }

  void set_auto_rotate(bool r) { this->auto_rotate_ = r; }
  void set_color_space(ColorSpace cs) { this->color_space_ = cs; }
  /// Calibrated appearance of palette entry i (0 black, 1 white, 2 yellow, 3 red, 4 blue, 5 green)
  void set_match_color(int i, uint8_t r, uint8_t g, uint8_t b);
  /// Switch the matching palette at runtime: "config" (YAML palette / built-in default),
  /// "measured" (first calibration photo), "aitjcize" (esp32-photoframe), "waveshare" (pure RGB).
  /// Takes effect with the next render. Returns false for an unknown name.
  bool set_palette_preset(const std::string &name);
  /// Page to switch to whenever a new image is ready (optional, for displays with pages)
  void set_page(display::DisplayPage *page) { this->page_ = page; }
  /// Show the six solid palette patches (black white yellow / red blue green) and refresh
  void show_calibration_pattern();
  /// Draw the current image into a display buffer: `lambda: id(gallery).draw(it);`
  void draw(display::Display &it);

  float get_setup_priority() const override { return setup_priority::LATE; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  /// Re-scan root_path for *.jpg/*.jpeg/*.bmp and fire list-changed callbacks.
  void refresh_list();
  const std::vector<std::string> &get_files() const { return this->files_; }
  /// Image currently on screen (survives reboots), "" if none yet
  std::string get_current() const;

  /// Schedule a file (name relative to root_path) for rendering.
  bool show(const std::string &filename);
  bool show_next();
  bool show_previous();
  /// true while an image is queued or being decoded/scaled/dithered (not the panel refresh itself)
  bool is_busy() const { return this->task_running_.load() || !this->pending_.empty(); }

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
  void preprocess_();
  void quantize_dizzy_(int strength);
  void prepare_palette_();
  int nearest_(const int v[3]) const;
  void present_(uint8_t *idx, int w, int h);
  void free_buffers_();
  template<typename T> void update_(T &field, T value) {
    if (field == value)
      return;
    field = value;
    this->settings_changed_();
  }
  void update_(float &field, float value) {  // UI round trips (x*100/100) aren't bit-exact
    if (std::fabs(field - value) < 0.001f)
      return;
    field = value;
    this->settings_changed_();
  }
  void settings_changed_();
  bool apply_palette_(const std::string &name);
  bool load_settings_(const std::string &path);
  std::string settings_path_(bool for_image) const;

  display::Display *display_{nullptr};
  display::DisplayPage *page_{nullptr};
  std::string root_path_{"/sdcard/images"};
  ScaleMode scale_mode_{SCALE_FIT};
  DitherMethod dither_method_{DITHER_FLOYD_STEINBERG};
  int error_clamp_{64};
  float dither_strength_{1.0f};
  float tone_compression_{1.0f};
  ToneCurve tone_curve_{TONE_LINEAR};
  float saturation_{1.3f};
  std::string palette_preset_{"config"};
  bool ready_{false};          // setup() done: setting changes trigger a re-render
  uint32_t rerender_at_{0};    // debounced re-render deadline (millis), 0 = none
  bool keep_settings_{false};  // pending render is a re-render: don't load a settings file
  bool auto_rotate_{true};
  ColorSpace color_space_{CS_LAB};
  // Colours used for the "which palette entry is closest" decision (order: black white
  // yellow red blue green). Built-in guess; YAML `palette:` overrides it (= preset "config").
#define EPAPER_GALLERY_DEFAULT_PALETTE \
  { {25, 25, 30}, {235, 235, 230}, {230, 210, 60}, {190, 40, 40}, {40, 60, 170}, {50, 140, 70} }
  int16_t match_rgb_[6][3] = EPAPER_GALLERY_DEFAULT_PALETTE;   // active, may be switched by preset
  int16_t config_rgb_[6][3] = EPAPER_GALLERY_DEFAULT_PALETTE;  // what preset "config" restores
#undef EPAPER_GALLERY_DEFAULT_PALETTE
  float match_lab_[6][3];     // same, in CIELAB (computed when rendering)

  std::vector<std::string> files_;
  int current_index_{-1};     // navigation position for show_next/previous
  std::string current_file_;  // image on screen (persisted), returned by get_current()
  ESPPreferenceObject last_pref_;
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
  // image currently on screen, kept so draw() can repaint it on every display update
  uint8_t *shown_idx_{nullptr};
  int shown_w_{0}, shown_h_{0};

  CallbackManager<void()> list_changed_cb_;
  CallbackManager<void(std::string)> image_shown_cb_;
  CallbackManager<void()> settings_loaded_cb_;
};

}  // namespace epaper_gallery
}  // namespace esphome
