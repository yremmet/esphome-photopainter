#pragma once

#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/display/display.h"

namespace esphome {
namespace epaper_gallery {

enum ScaleMode { SCALE_FIT, SCALE_FILL };

/**
 * Loads JPEG/BMP files from a mounted filesystem (SD card), scales them to the
 * display, Floyd–Steinberg-dithers them to the 6-colour Spectra E6 palette and
 * pushes the result to the display buffer.
 *
 * Everything runs in the main loop and blocks for a few seconds per image;
 * the watchdog is fed regularly. An e-paper refresh takes ~25 s anyway.
 */
class EPaperGallery : public Component {
 public:
  void set_display(display::Display *d) { this->display_ = d; }
  void set_root_path(const std::string &p) { this->root_path_ = p; }
  void set_scale_mode(ScaleMode m) { this->scale_mode_ = m; }
  void set_dither(bool d) { this->dither_ = d; }
  void set_auto_rotate(bool r) { this->auto_rotate_ = r; }

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

  void add_on_list_changed_callback(std::function<void()> &&cb) { this->list_changed_cb_.add(std::move(cb)); }
  void add_on_image_shown_callback(std::function<void(std::string)> &&cb) {
    this->image_shown_cb_.add(std::move(cb));
  }

  /// Internal: JPEGDEC draw callback target (void* to avoid leaking JPEGDEC.h into the header)
  void jpeg_block(void *draw);

 protected:
  bool render_(const std::string &path);
  bool decode_jpeg_(uint8_t *data, size_t size);
  bool decode_bmp_(uint8_t *data, size_t size);
  void resample_();
  void dither_and_draw_();
  void free_buffers_();

  display::Display *display_{nullptr};
  std::string root_path_{"/sdcard/images"};
  ScaleMode scale_mode_{SCALE_FIT};
  bool dither_{true};
  bool auto_rotate_{true};

  std::vector<std::string> files_;
  int current_index_{-1};
  std::string pending_;

  // decoded source image, RGB888
  uint8_t *src_{nullptr};
  int src_w_{0}, src_h_{0};
  // display-sized working image, RGB888
  uint8_t *dst_{nullptr};
  int dst_w_{0}, dst_h_{0};

  CallbackManager<void()> list_changed_cb_;
  CallbackManager<void(std::string)> image_shown_cb_;
};

}  // namespace epaper_gallery
}  // namespace esphome
