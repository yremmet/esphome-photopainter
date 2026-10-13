#pragma once

#include <initializer_list>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/display/display_buffer.h"
#include "esphome/components/spi/spi.h"

namespace esphome::epaper_154_gray4 {

/// Ink level of a colour, 0 = white .. 3 = black. Like ESPHome's binary e-paper drivers, a colour is
/// "ink": COLOR_ON (and any bright colour) is black, COLOR_OFF white, so anti-aliased fonts blend right.
/// Rec.601 luma, quantised to the nearest of 0/85/170/255.
inline uint8_t ink_level(Color c) {
  const uint32_t luma = (77u * c.r + 150u * c.g + 29u * c.b + 128u) >> 8;
  return (uint8_t) ((luma * 3u + 127u) / 255u);
}

/// One RAM plane byte (8 pixels, MSB = leftmost) from two framebuffer bytes (4 pixels each, 2 bit,
/// MSB first): bit 1 of each level for bit == 1, bit 0 for bit == 0.
inline uint8_t plane_byte(const uint8_t *src, uint8_t bit) {
  uint8_t out = 0;
  for (int b = 0; b < 2; b++)
    for (int shift = 6; shift >= 0; shift -= 2)
      out = (uint8_t) ((out << 1) | ((src[b] >> (shift + bit)) & 1));
  return out;
}

/// A partial update runs in two steps, both without flashing the whole panel:
///  1. black/white OTP partial (differential): pixels that change go to white, or to black for level 3.
///     `old_plane`/`new_plane` (1 = white) go to RAM 0x26/0x24. Unchanged pixels report old == new and are
///     not driven. A pixel that is gray now and changes reports the opposite of its new colour, so it is
///     driven all the way (otherwise light gray counts as "already white" and stays as a ghost): `forced`.
///  2. gray pulse: pixels that become gray get the white -> gray pulse of the 4-gray LUT. `pulse_r`/`pulse_bw`
///     is their level as LUT index (1 light, 2 dark), 0 = not driven. `pulse` = any pixel needs it.
struct PartialBytes {
  uint8_t old_plane, new_plane, pulse_r, pulse_bw;
  bool forced, pulse;
};

/// PartialBytes for 8 pixels from what the panel shows (`shown`) and the new frame (`now`), 2 bit each.
inline PartialBytes partial_bytes(const uint8_t *shown, const uint8_t *now) {
  PartialBytes o{0, 0, 0, 0, false, false};
  for (int b = 0; b < 2; b++)
    for (int shift = 6; shift >= 0; shift -= 2) {
      const uint8_t s = (shown[b] >> shift) & 3, t = (now[b] >> shift) & 3;
      const uint8_t new_white = t != 3;
      uint8_t old_white = new_white, pulse = 0;
      if (s != t) {
        const bool gray_now = s == 1 || s == 2;
        old_white = gray_now ? !new_white : s == 0;
        o.forced |= gray_now;
        if (t == 1 || t == 2)
          pulse = t;
      }
      o.old_plane = (uint8_t) ((o.old_plane << 1) | old_white);
      o.new_plane = (uint8_t) ((o.new_plane << 1) | new_white);
      o.pulse_r = (uint8_t) ((o.pulse_r << 1) | (pulse >> 1));
      o.pulse_bw = (uint8_t) ((o.pulse_bw << 1) | (pulse & 1));
      o.pulse |= pulse != 0;
    }
  return o;
}

/// Waveshare 1.54" V2 / Good Display GDEH0154D67 (SSD1681, 200x200) with 4 gray levels.
/// Full update = 4-gray refresh with the Good Display LUT (flashes the whole panel),
/// partial update = black/white OTP partial plus a gray pulse for pixels that become gray (no flash).
class EPaper154Gray4 : public display::DisplayBuffer,
                       public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                             spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_2MHZ> {
 public:
  void set_dc_pin(GPIOPin *pin) { this->dc_pin_ = pin; }
  void set_reset_pin(GPIOPin *pin) { this->reset_pin_ = pin; }
  void set_busy_pin(GPIOPin *pin) { this->busy_pin_ = pin; }
  void set_reset_duration(uint32_t ms) { this->reset_duration_ = ms; }
  void set_full_update_every(uint32_t n) { this->full_update_every_ = n; }
  void set_cleanup_passes(uint8_t n) { this->cleanup_passes_ = n; }

  /// Make the next update a 4-gray full refresh.
  void full_update_next() { this->at_update_ = 0; }
  /// Draw the next partial update in black/white only (gray > 50 % = black): skips the gray pulse,
  /// for faster animation frames.
  void bw_only_next() { this->bw_only_next_ = true; }
  /// True while the panel is still refreshing (update() already returned).
  bool is_refreshing() const { return this->refreshing_; }

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }
  void on_safe_shutdown() override;

  void fill(Color color) override;
  display::DisplayType get_display_type() override { return display::DisplayType::DISPLAY_TYPE_GRAYSCALE; }

 protected:
  static constexpr int WIDTH = 200;
  static constexpr int HEIGHT = 200;
  static constexpr size_t BUFFER_LEN = WIDTH * HEIGHT / 4;  // 2 bit per pixel

  int get_width_internal() override { return WIDTH; }
  int get_height_internal() override { return HEIGHT; }
  void draw_absolute_pixel_internal(int x, int y, Color color) override;

  void command_(uint8_t cmd);
  void cmd_data_(uint8_t cmd, std::initializer_list<uint8_t> data);
  void cmd_data_(uint8_t cmd, const uint8_t *data, size_t len);
  bool wait_idle_(uint32_t timeout_ms);
  void reset_();
  void set_ram_area_();
  void write_plane_(uint8_t cmd, uint8_t bit);
  void write_partial_plane_(uint8_t cmd, uint8_t PartialBytes::*field);
  void send_gray_();
  void send_partial_();
  void send_pulse_();
  void start_partial_();
  void start_wait_(const char *what, uint32_t timeout_ms);
  void finish_partial_();

  GPIOPin *dc_pin_{nullptr};
  GPIOPin *reset_pin_{nullptr};
  GPIOPin *busy_pin_{nullptr};
  uint32_t reset_duration_{200};
  uint32_t full_update_every_{1};
  uint32_t at_update_{0};
  uint8_t cleanup_passes_{1};
  uint8_t cleanup_done_{0};
  uint8_t *shown_{nullptr};  // what the panel shows, 2 bit per pixel: "old" data for partial updates
  bool repeat_{false};       // run step 1 of the partial update again (cleanup pass)
  bool pulse_next_{false};   // step 2 (gray pulse) follows
  bool bw_only_next_{false};
  uint32_t refresh_start_{0};
  uint32_t refresh_timeout_{0};
  const char *refresh_what_{""};
  bool refreshing_{false};
  bool pending_{false};
};

template<typename... Ts> class FullUpdateNextAction : public Action<Ts...>, public Parented<EPaper154Gray4> {
 public:
  void play(const Ts &...x) override { this->parent_->full_update_next(); }
};

}  // namespace esphome::epaper_154_gray4
