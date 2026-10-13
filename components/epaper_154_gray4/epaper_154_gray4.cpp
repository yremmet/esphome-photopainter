#include "epaper_154_gray4.h"

#include <cinttypes>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::epaper_154_gray4 {

static const char *const TAG = "epaper_154_gray4";

// 4-gray waveform for GDEY0154D67 (SSD1681), verbatim from GxEPD2_4G
// (https://github.com/ZinggJM/GxEPD2_4G, src/gdey/GxEPD2_154_GDEY0154D67.cpp, lut_4G, commit de00616),
// which takes it from the Good Display demo. Good Display does not publish its 4Gray demo for this panel.
// LUT index = (R RAM bit << 1) | (B/W RAM bit)  (SSD1681 datasheet rev 0.13, table 6-4/6-5).
// Only bytes 0..152 are sent with 0x32, exactly like GxEPD2_4G; bytes 153..158 (0x3F/0x03/0x04/0x2C
// values) are kept for reference but not written, so those registers stay at their reset values.
static const uint8_t LUT_4GRAY[159] = {
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // VS L0 white
    0x08, 0x48, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // VS L1 light gray
    0x02, 0x48, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // VS L2 dark gray
    0x20, 0x48, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // VS L3 black
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // VS L4 (vcom)
    0x0A, 0x19, 0x00, 0x03, 0x08, 0x00, 0x00,                                // TP 0 RP0
    0x14, 0x01, 0x00, 0x14, 0x01, 0x00, 0x03,                                // TP 1 RP1
    0x0A, 0x03, 0x00, 0x08, 0x19, 0x00, 0x00,                                // TP 2 RP2
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,                                // TP 3 RP3
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                // TP 4 RP4
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                // TP 5 RP5
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                // TP 6 RP6
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                // TP 7 RP7
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                // TP 8 RP8
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                // TP 9 RP9
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                // TP 10 RP10
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                // TP 11 RP11
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,                    // FR, XON @144
    0x22, 0x17, 0x41, 0x00, 0x32, 0x1C,                                      // @153, not sent
};
static const size_t LUT_4GRAY_SEND = 153;

// Busy timeouts. Partial: waveshare_epaper's value for this panel. Gray: generous, the 4-gray
// waveform runs longer than the OTP full refresh.
static const uint32_t TIMEOUT_PARTIAL_MS = 2500;
static const uint32_t TIMEOUT_GRAY_MS = 10000;
static const uint32_t TIMEOUT_SHORT_MS = 500;

void EPaper154Gray4::setup() {
  this->init_internal_(BUFFER_LEN);  // RAMAllocator: PSRAM if available
  this->shown_ = RAMAllocator<uint8_t>().allocate(BUFFER_LEN);
  if (this->buffer_ == nullptr || this->shown_ == nullptr) {
    this->mark_failed();
    return;
  }
  memset(this->shown_, 0x00, BUFFER_LEN);  // white

  this->dc_pin_->setup();
  this->dc_pin_->digital_write(false);
  this->reset_pin_->setup();
  this->reset_pin_->digital_write(true);
  this->busy_pin_->setup();
  this->spi_setup();

  // Like waveshare_epaper 1.54inv2 with a reset pin: deep sleep until the first update, which
  // resets and initialises the panel anyway.
  this->reset_();
  this->wait_idle_(TIMEOUT_SHORT_MS);
  this->cmd_data_(0x10, {0x01});
  this->disable_loop();
}

void EPaper154Gray4::update() {
  if (this->refreshing_) {
    this->pending_ = true;  // redraw as soon as the running refresh is done
    return;
  }
  this->do_update_();

  const bool gray = this->at_update_ == 0;
  this->at_update_ = (this->at_update_ + 1) % this->full_update_every_;

  if (gray) {
    this->send_gray_();
    memcpy(this->shown_, this->buffer_, BUFFER_LEN);
    this->shown_gray_ = true;
    this->cleanup_done_ = 0;
    this->start_wait_(true);
  } else {
    this->start_partial_();
  }
}

// After a gray refresh, the first `cleanup_passes` partial updates drive every formerly gray pixel
// (see old_byte()); the same frame is sent again until all passes are done.
void EPaper154Gray4::start_partial_() {
  this->send_partial_();
  if (this->shown_gray_ && ++this->cleanup_done_ < this->cleanup_passes_) {
    this->repeat_ = true;
  } else {
    // The panel now shows the frame in black/white
    for (size_t i = 0; i < BUFFER_LEN; i++) {
      uint8_t b = this->buffer_[i], out = 0;
      for (int shift = 6; shift >= 0; shift -= 2)
        out |= (((b >> shift) & 2) ? 3 : 0) << shift;
      this->shown_[i] = out;
    }
    this->shown_gray_ = false;
    this->cleanup_done_ = 0;
  }
  this->start_wait_(false);
}

void EPaper154Gray4::start_wait_(bool gray) {
  this->refresh_gray_ = gray;
  this->refresh_start_ = millis();
  this->refresh_timeout_ = gray ? TIMEOUT_GRAY_MS : TIMEOUT_PARTIAL_MS;
  this->refreshing_ = true;
  this->enable_loop();
  ESP_LOGD(TAG, "%s refresh started", gray ? "Gray" : "Partial");
}

// Waits for BUSY without blocking: the refresh itself runs while ESPHome keeps looping.
void EPaper154Gray4::loop() {
  if (!this->refreshing_) {
    this->disable_loop();
    return;
  }
  const uint32_t elapsed = millis() - this->refresh_start_;
  if (this->busy_pin_->digital_read()) {
    if (elapsed < this->refresh_timeout_)
      return;
    ESP_LOGE(TAG, "Timeout waiting for the refresh to finish (%" PRIu32 " ms)", elapsed);
    this->status_set_warning();
  } else {
    ESP_LOGD(TAG, "%s refresh took %" PRIu32 " ms", this->refresh_gray_ ? "Gray" : "Partial", elapsed);
    this->status_clear_warning();
  }
  this->cmd_data_(0x10, {0x01});  // deep sleep, as waveshare_epaper does after every update
  this->refreshing_ = false;
  this->disable_loop();
  if (this->repeat_) {
    this->repeat_ = false;
    ESP_LOGD(TAG, "Cleanup pass %u/%u", this->cleanup_done_ + 1, this->cleanup_passes_);
    this->start_partial_();
    return;
  }
  if (this->pending_) {
    this->pending_ = false;
    this->update();
  }
}

void EPaper154Gray4::on_safe_shutdown() {
  if (!this->refreshing_)
    return;  // panel already in deep sleep
  this->wait_idle_(this->refresh_timeout_);
  this->cmd_data_(0x10, {0x01});
  this->refreshing_ = false;
}

// Register sequence of GxEPD2_4G GxEPD2_154_GDEY0154D67: _Init_4G() + writeImage_4G() + _Update_4G().
// 0x22 = 0xC7: "Enable clock, Enable Analog, Display with DISPLAY Mode 1, Disable Analog, Disable OSC"
// (datasheet 0x22 table). It neither loads the temperature nor an OTP LUT, so the LUT just written with
// 0x32 is used. 0xCF would be the same with Display Mode 2 (the differential/partial mode), which is
// not meant for driving every pixel through all four LUTs.
void EPaper154Gray4::send_gray_() {
  this->reset_();
  delay(10);  // NOLINT  10 ms according to specs (GxEPD2)
  this->command_(0x12);  // SWRESET
  delay(10);  // NOLINT
  this->cmd_data_(0x01, {0xC7, 0x00, 0x00});  // driver output control: 200 gate lines
  this->cmd_data_(0x3C, {0x00});              // border waveform
  this->cmd_data_(0x18, {0x80});              // internal temperature sensor
  this->set_ram_area_();
  this->cmd_data_(0x32, LUT_4GRAY, LUT_4GRAY_SEND);
  // GxEPD2 clears both RAMs here; skipped, both planes are fully written right after.
  this->write_plane_(0x26, 1, false);  // R RAM   = level bit 1
  this->write_plane_(0x24, 0, false);  // B/W RAM = level bit 0
  this->cmd_data_(0x22, {0xC7});
  this->command_(0x20);  // master activation
}

// Black/white partial update, register sequence of waveshare_epaper WaveshareEPaperTypeA for 1.54inv2.
// Differences: the 30 byte SSD1608 LUT is not written (0x22 = 0xFF has the "load LUT" bit set, so the
// OTP waveform replaces it anyway), and 0x26 gets the previously shown frame (see old_byte()), so the
// differential waveform compares against the real old image.
void EPaper154Gray4::send_partial_() {
  this->reset_();
  this->wait_idle_(TIMEOUT_SHORT_MS);
  this->cmd_data_(0x01, {0xC7, 0x00, 0x00});  // driver output control
  this->cmd_data_(0x0C, {0xD7, 0xD6, 0x9D});  // booster soft start
  this->cmd_data_(0x2C, {0xA8});              // VCOM
  this->cmd_data_(0x3A, {0x1A});              // dummy line period
  this->cmd_data_(0x3B, {0x08});              // gate time
  this->cmd_data_(0x11, {0x03});              // data entry: x+, y+
  this->wait_idle_(TIMEOUT_SHORT_MS);
  this->set_ram_area_();
  this->write_old_();
  this->write_plane_(0x24, 1, true);  // level >= 2 (>50 %) -> black; B/W RAM 1 = white
  this->cmd_data_(0x22, {0xFF});
  this->command_(0x20);  // master activation
  this->command_(0xFF);  // terminate frame read write (as waveshare_epaper)
}

void EPaper154Gray4::set_ram_area_() {
  this->cmd_data_(0x11, {0x03});                                 // x+, y+
  this->cmd_data_(0x44, {0x00, (WIDTH - 1) >> 3});               // x start/end in bytes
  this->cmd_data_(0x45, {0x00, 0x00, HEIGHT - 1, (HEIGHT - 1) >> 8});  // y start/end
  this->cmd_data_(0x4E, {0x00});
  this->cmd_data_(0x4F, {0x00, 0x00});
}

void EPaper154Gray4::write_plane_(uint8_t cmd, uint8_t bit, bool invert) {
  this->cmd_data_(0x4E, {0x00});  // start each plane at the window origin
  this->cmd_data_(0x4F, {0x00, 0x00});
  this->command_(cmd);
  uint8_t row[WIDTH / 8];
  this->dc_pin_->digital_write(true);
  this->enable();
  for (size_t y = 0; y < HEIGHT; y++) {
    const uint8_t *src = this->buffer_ + y * (WIDTH / 4);
    for (size_t i = 0; i < sizeof(row); i++) {
      const uint8_t b = plane_byte(src + 2 * i, bit);
      row[i] = invert ? (uint8_t) ~b : b;
    }
    this->write_array(row, sizeof(row));
  }
  this->disable();
}

void EPaper154Gray4::write_old_() {
  this->cmd_data_(0x4E, {0x00});
  this->cmd_data_(0x4F, {0x00, 0x00});
  this->command_(0x26);
  uint8_t row[WIDTH / 8];
  this->dc_pin_->digital_write(true);
  this->enable();
  for (size_t y = 0; y < HEIGHT; y++) {
    const size_t off = y * (WIDTH / 4);
    for (size_t i = 0; i < sizeof(row); i++)
      row[i] = old_byte(this->shown_ + off + 2 * i, this->buffer_ + off + 2 * i);
    this->write_array(row, sizeof(row));
  }
  this->disable();
}

void EPaper154Gray4::command_(uint8_t cmd) {
  this->dc_pin_->digital_write(false);
  this->enable();
  this->write_byte(cmd);
  this->disable();
}

void EPaper154Gray4::cmd_data_(uint8_t cmd, const uint8_t *data, size_t len) {
  this->command_(cmd);
  this->dc_pin_->digital_write(true);
  this->enable();
  this->write_array(data, len);
  this->disable();
}

void EPaper154Gray4::cmd_data_(uint8_t cmd, std::initializer_list<uint8_t> data) {
  this->cmd_data_(cmd, data.begin(), data.size());
}

// Short, bounded waits only (after reset, at shutdown); the refresh itself is awaited in loop().
bool EPaper154Gray4::wait_idle_(uint32_t timeout_ms) {
  const uint32_t start = millis();
  while (this->busy_pin_->digital_read()) {
    if (millis() - start > timeout_ms) {
      ESP_LOGE(TAG, "Timeout waiting for BUSY");
      return false;
    }
    App.feed_wdt();
    delay(1);  // NOLINT
  }
  return true;
}

void EPaper154Gray4::reset_() {
  this->reset_pin_->digital_write(false);
  delay(this->reset_duration_);  // NOLINT
  this->reset_pin_->digital_write(true);
  delay(20);  // NOLINT
}

void EPaper154Gray4::fill(Color color) {
  if (this->get_clipping().is_set()) {
    display::Display::fill(color);
    return;
  }
  const uint8_t l = ink_level(color);
  memset(this->buffer_, l | (l << 2) | (l << 4) | (l << 6), BUFFER_LEN);
}

void HOT EPaper154Gray4::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT)
    return;
  const size_t pos = (y * WIDTH + x) / 4;
  const uint8_t shift = 6 - 2 * (x & 3);
  this->buffer_[pos] = (this->buffer_[pos] & ~(0x03 << shift)) | (ink_level(color) << shift);
}

void EPaper154Gray4::dump_config() {
  LOG_DISPLAY("", "E-Paper 1.54\" 4-gray (SSD1681)", this);
  ESP_LOGCONFIG(TAG, "  Full Update Every: %" PRIu32, this->full_update_every_);
  ESP_LOGCONFIG(TAG, "  Cleanup Passes: %u", this->cleanup_passes_);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  LOG_PIN("  DC Pin: ", this->dc_pin_);
  LOG_PIN("  Busy Pin: ", this->busy_pin_);
  LOG_UPDATE_INTERVAL(this);
}

}  // namespace esphome::epaper_154_gray4
