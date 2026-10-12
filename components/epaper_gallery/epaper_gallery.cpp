#include "epaper_gallery.h"

#include <JPEGDEC.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <dirent.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome {
namespace epaper_gallery {

static const char *const TAG = "epaper_gallery";

// Native Spectra E6 colours. The epaper_spi driver maps these pure RGB values
// 1:1 to the panel's colour codes, so we must draw exactly these.
static const uint8_t PALETTE[6][3] = {
    {0, 0, 0},        // black
    {255, 255, 255},  // white
    {255, 255, 0},    // yellow
    {255, 0, 0},      // red
    {0, 0, 255},      // blue
    {0, 255, 0},      // green
};

// Colours used for the "which palette entry is closest" decision. The real
// panel pigments are far less saturated than pure RGB; using something closer
// to reality here makes photos look noticeably better. Tweak to taste.
static const int16_t DEFAULT_MATCH_PALETTE[6][3] = {
    {25, 25, 30},     // black
    {235, 235, 230},  // white
    {230, 210, 60},   // yellow
    {190, 40, 40},    // red
    {40, 60, 170},    // blue
    {50, 140, 70},    // green
};

static constexpr int RENDER_TASK_CORE = 1;
static constexpr uint32_t RENDER_TASK_STACK = 12 * 1024;

// Feed the task watchdog only if the calling task is subscribed to it
// (the main loop is, our render task is not).
static inline void feed_wdt_safe() {
  if (esp_task_wdt_status(nullptr) == ESP_OK)
    feed_wdt_safe();
}

static void render_task_entry(void *arg) {
  static_cast<EPaperGallery *>(arg)->render_task_body_();
  vTaskDelete(nullptr);
}

static void *psram_alloc(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == nullptr)
    p = malloc(n);
  return p;
}

static inline uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24); }
static inline uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static bool ends_with(const std::string &s, const char *suffix) {
  size_t n = strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// ---------------------------------------------------------------- lifecycle

void EPaperGallery::setup() {
  // apply defaults for any palette entry the YAML did not override (set_match_color sets a flag via r|g|b != -1)
  for (int i = 0; i < 6; i++) {
    if (this->match_rgb_[i][0] < 0) {
      for (int c = 0; c < 3; c++)
        this->match_rgb_[i][c] = DEFAULT_MATCH_PALETTE[i][c];
    }
  }
  this->refresh_list();
}

void EPaperGallery::set_match_color(int i, uint8_t r, uint8_t g, uint8_t b) {
  if (i < 0 || i > 5)
    return;
  this->match_rgb_[i][0] = r;
  this->match_rgb_[i][1] = g;
  this->match_rgb_[i][2] = b;
}

void EPaperGallery::draw_calibration_pattern() {
  const int w = this->display_->get_width(), h = this->display_->get_height();
  const int cols = 3, rows = 2;
  const int pw = w / cols, ph = h / rows;
  for (int i = 0; i < 6; i++) {
    const int x = (i % cols) * pw, y = (i / cols) * ph;
    this->display_->filled_rectangle(x, y, pw, ph, Color(PALETTE[i][0], PALETTE[i][1], PALETTE[i][2]));
  }
  ESP_LOGI(TAG, "Calibration pattern drawn: black white yellow / red blue green");
}

// ---------------------------------------------------------------- colour maths

// sRGB (0..255) -> CIELAB (D65). Linearisation via LUT, cube root via cbrtf.
static float srgb_lin_lut[256];
static bool srgb_lut_ready = false;

static inline float lab_f(float t) { return t > 0.008856f ? cbrtf(t) : (7.787f * t + 16.0f / 116.0f); }

static void rgb_to_lab(int r, int g, int b, float out[3]) {
  if (!srgb_lut_ready) {
    for (int i = 0; i < 256; i++) {
      float c = i / 255.0f;
      srgb_lin_lut[i] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    srgb_lut_ready = true;
  }
  const float R = srgb_lin_lut[r], G = srgb_lin_lut[g], B = srgb_lin_lut[b];
  const float X = (0.4124f * R + 0.3576f * G + 0.1805f * B) / 0.95047f;
  const float Y = (0.2126f * R + 0.7152f * G + 0.0722f * B);
  const float Z = (0.0193f * R + 0.1192f * G + 0.9505f * B) / 1.08883f;
  const float fx = lab_f(X), fy = lab_f(Y), fz = lab_f(Z);
  out[0] = 116.0f * fy - 16.0f;
  out[1] = 500.0f * (fx - fy);
  out[2] = 200.0f * (fy - fz);
}

void EPaperGallery::prepare_palette_() {
  for (int i = 0; i < 6; i++)
    rgb_to_lab(this->match_rgb_[i][0], this->match_rgb_[i][1], this->match_rgb_[i][2], this->match_lab_[i]);
}

int EPaperGallery::nearest_(const int v[3]) const {
  int best = 0;
  float best_d = 1e30f;
  if (this->color_space_ == CS_LAB) {
    float lab[3];
    rgb_to_lab(v[0], v[1], v[2], lab);
    for (int k = 0; k < 6; k++) {
      const float dl = lab[0] - this->match_lab_[k][0];
      const float da = lab[1] - this->match_lab_[k][1];
      const float db = lab[2] - this->match_lab_[k][2];
      const float d = dl * dl + da * da + db * db;
      if (d < best_d) {
        best_d = d;
        best = k;
      }
    }
  } else {
    for (int k = 0; k < 6; k++) {
      const float dr = v[0] - this->match_rgb_[k][0];
      const float dg = v[1] - this->match_rgb_[k][1];
      const float db = v[2] - this->match_rgb_[k][2];
      const float d = dr * dr + dg * dg + db * db;
      if (d < best_d) {
        best_d = d;
        best = k;
      }
    }
  }
  return best;
}

void EPaperGallery::dump_config() {
  ESP_LOGCONFIG(TAG, "E-Paper Gallery:");
  ESP_LOGCONFIG(TAG, "  Root path: %s", this->root_path_.c_str());
  ESP_LOGCONFIG(TAG, "  Scale: %s", this->scale_mode_ == SCALE_FILL ? "fill" : "fit");
  ESP_LOGCONFIG(TAG, "  Dither: %s", YESNO(this->dither_));
  ESP_LOGCONFIG(TAG, "  Auto-rotate: %s", YESNO(this->auto_rotate_));
  ESP_LOGCONFIG(TAG, "  Colour space: %s", this->color_space_ == CS_LAB ? "lab" : "rgb");
  for (int i = 0; i < 6; i++)
    ESP_LOGCONFIG(TAG, "  Palette[%d]: #%02X%02X%02X", i, this->match_rgb_[i][0], this->match_rgb_[i][1],
                  this->match_rgb_[i][2]);
  ESP_LOGCONFIG(TAG, "  Images found: %u", (unsigned) this->files_.size());
}

void EPaperGallery::loop() {
  // 1) a finished render -> push to the display from the main loop (SPI lives here)
  if (this->task_done_.load()) {
    this->task_done_.store(false);
    this->task_running_.store(false);
    if (this->task_ok_) {
      this->blit_and_update_();
      this->image_shown_cb_.call(this->task_file_);
    }
    this->free_buffers_();
  }

  // 2) start the next render if one is pending and nothing is running
  if (this->pending_.empty() || this->task_running_.load())
    return;

  this->task_file_ = this->pending_;
  this->pending_.clear();

  auto it = std::find(this->files_.begin(), this->files_.end(), this->task_file_);
  if (it != this->files_.end())
    this->current_index_ = it - this->files_.begin();

  this->dst_w_ = this->display_->get_width();
  this->dst_h_ = this->display_->get_height();
  this->task_ok_ = false;
  this->task_running_.store(true);
  BaseType_t ok = xTaskCreatePinnedToCore(render_task_entry, "epaper_render", RENDER_TASK_STACK, this,
                                          tskIDLE_PRIORITY + 1, nullptr, RENDER_TASK_CORE);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Could not create render task");
    this->task_running_.store(false);
  }
}

void EPaperGallery::render_task_body_() {
  this->task_ok_ = this->render_(this->root_path_ + "/" + this->task_file_);
  this->task_done_.store(true);
}

// ---------------------------------------------------------------- library

void EPaperGallery::refresh_list() {
  this->files_.clear();
  DIR *dir = opendir(this->root_path_.c_str());
  if (dir == nullptr) {
    ESP_LOGW(TAG, "Cannot open %s (SD not mounted or folder missing?)", this->root_path_.c_str());
    this->list_changed_cb_.call();
    return;
  }
  struct dirent *e;
  while ((e = readdir(dir)) != nullptr) {
    if (e->d_type == DT_DIR)
      continue;
    std::string name = e->d_name;
    if (name.empty() || name[0] == '.')
      continue;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (ends_with(lower, ".jpg") || ends_with(lower, ".jpeg") || ends_with(lower, ".bmp"))
      this->files_.push_back(name);
  }
  closedir(dir);
  std::sort(this->files_.begin(), this->files_.end());
  ESP_LOGI(TAG, "%u images in %s", (unsigned) this->files_.size(), this->root_path_.c_str());
  this->list_changed_cb_.call();
}

std::string EPaperGallery::get_current() const {
  if (this->current_index_ < 0 || this->current_index_ >= (int) this->files_.size())
    return "";
  return this->files_[this->current_index_];
}

bool EPaperGallery::show(const std::string &filename) {
  if (filename.empty())
    return false;
  this->pending_ = filename;
  return true;
}

bool EPaperGallery::show_next() {
  if (this->files_.empty())
    return false;
  this->current_index_ = (this->current_index_ + 1) % (int) this->files_.size();
  return this->show(this->files_[this->current_index_]);
}

bool EPaperGallery::show_previous() {
  if (this->files_.empty())
    return false;
  int n = this->files_.size();
  this->current_index_ = (this->current_index_ - 1 + n) % n;
  return this->show(this->files_[this->current_index_]);
}

// ---------------------------------------------------------------- pipeline

void EPaperGallery::free_buffers_() {
  if (this->src_ != nullptr) {
    free(this->src_);
    this->src_ = nullptr;
  }
  if (this->dst_ != nullptr) {
    free(this->dst_);
    this->dst_ = nullptr;
  }
  if (this->idx_ != nullptr) {
    free(this->idx_);
    this->idx_ = nullptr;
  }
}

bool EPaperGallery::render_(const std::string &path) {
  const uint32_t t0 = millis();
  ESP_LOGI(TAG, "Rendering %s", path.c_str());

  FILE *fp = fopen(path.c_str(), "rb");
  if (fp == nullptr) {
    ESP_LOGE(TAG, "Cannot open %s", path.c_str());
    return false;
  }
  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (size <= 4) {
    fclose(fp);
    ESP_LOGE(TAG, "Empty file");
    return false;
  }
  uint8_t *data = (uint8_t *) psram_alloc(size);
  if (data == nullptr) {
    fclose(fp);
    ESP_LOGE(TAG, "Out of memory for %ld bytes", size);
    return false;
  }
  size_t rd = fread(data, 1, size, fp);
  fclose(fp);
  if (rd != (size_t) size) {
    free(data);
    ESP_LOGE(TAG, "Short read (%u/%ld)", (unsigned) rd, size);
    return false;
  }
  feed_wdt_safe();

  bool ok = false;
  if (data[0] == 'B' && data[1] == 'M') {
    ok = this->decode_bmp_(data, size);
  } else if (data[0] == 0xFF && data[1] == 0xD8) {
    ok = this->decode_jpeg_(data, size);
  } else {
    ESP_LOGE(TAG, "Unsupported format (not JPEG/BMP)");
  }
  free(data);
  if (!ok) {
    this->free_buffers_();
    return false;
  }
  const uint32_t t1 = millis();
  ESP_LOGI(TAG, "Decoded %dx%d in %u ms", this->src_w_, this->src_h_, (unsigned) (t1 - t0));
  feed_wdt_safe();

  this->resample_();
  free(this->src_);
  this->src_ = nullptr;
  const uint32_t t2 = millis();
  ESP_LOGI(TAG, "Resampled to %dx%d in %u ms", this->dst_w_, this->dst_h_, (unsigned) (t2 - t1));
  feed_wdt_safe();

  this->dither_();
  free(this->dst_);
  this->dst_ = nullptr;
  const uint32_t t3 = millis();
  ESP_LOGI(TAG, "Dithered in %u ms (core %d), handing over to main loop", (unsigned) (t3 - t2), xPortGetCoreID());
  return this->idx_ != nullptr;
}

// Runs in the main loop: copy palette indices into the display buffer and refresh.
void EPaperGallery::blit_and_update_() {
  const uint32_t t0 = millis();
  const int w = this->dst_w_, h = this->dst_h_;
  for (int y = 0; y < h; y++) {
    const uint8_t *row = this->idx_ + (size_t) y * w;
    for (int x = 0; x < w; x++) {
      const uint8_t *p = PALETTE[row[x]];
      this->display_->draw_pixel_at(x, y, Color(p[0], p[1], p[2]));
    }
    if ((y & 31) == 0)
      feed_wdt_safe();
  }
  ESP_LOGI(TAG, "Blitted in %u ms, triggering display update", (unsigned) (millis() - t0));
  // display must have auto_clear_enabled: false and no lambda, otherwise the
  // buffer we just filled would be wiped here.
  this->display_->update();
}

// ---------------------------------------------------------------- JPEG

static int jpeg_draw_cb(JPEGDRAW *d) {
  auto *self = static_cast<EPaperGallery *>(d->pUser);
  self->jpeg_block(d);
  return 1;
}

void EPaperGallery::jpeg_block(void *draw) {
  auto *d = static_cast<JPEGDRAW *>(draw);
  feed_wdt_safe();
  const uint16_t *px = d->pPixels;  // RGB565 little endian
  for (int y = 0; y < d->iHeight; y++) {
    int dy = d->y + y;
    if (dy >= this->src_h_)
      break;
    uint8_t *o = this->src_ + ((size_t) dy * this->src_w_ + d->x) * 3;
    for (int x = 0; x < d->iWidth; x++) {
      int dx = d->x + x;
      if (dx >= this->src_w_)
        break;
      uint16_t v = px[y * d->iWidth + x];
      o[0] = ((v >> 11) & 0x1F) * 255 / 31;
      o[1] = ((v >> 5) & 0x3F) * 255 / 63;
      o[2] = (v & 0x1F) * 255 / 31;
      o += 3;
    }
  }
}

bool EPaperGallery::decode_jpeg_(uint8_t *data, size_t size) {
  // JPEGDEC object is large; keep it off the stack
  auto *jpeg = new JPEGDEC();
  if (!jpeg->openRAM(data, size, jpeg_draw_cb)) {
    ESP_LOGE(TAG, "JPEG open failed (%d) - progressive JPEGs are not supported", jpeg->getLastError());
    delete jpeg;
    return false;
  }
  const int w = jpeg->getWidth();
  const int h = jpeg->getHeight();
  const int tw = this->dst_w_;
  const int th = this->dst_h_;

  // Let the decoder downscale by 2/4/8 while the result still covers the
  // display in both dimensions (saves a lot of RAM and time on phone photos).
  int shift = 0;
  while (shift < 3 && (w >> (shift + 1)) >= std::max(tw, th) && (h >> (shift + 1)) >= std::max(tw, th))
    shift++;
  static const int OPTS[] = {0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH};

  this->src_w_ = w >> shift;
  this->src_h_ = h >> shift;
  this->src_ = (uint8_t *) psram_alloc((size_t) this->src_w_ * this->src_h_ * 3);
  if (this->src_ == nullptr) {
    ESP_LOGE(TAG, "Out of memory for %dx%d source", this->src_w_, this->src_h_);
    jpeg->close();
    delete jpeg;
    return false;
  }
  jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
  jpeg->setUserPointer(this);
  int ok = jpeg->decode(0, 0, OPTS[shift]);
  int err = jpeg->getLastError();
  jpeg->close();
  delete jpeg;
  if (!ok) {
    ESP_LOGE(TAG, "JPEG decode failed (%d)", err);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- BMP

bool EPaperGallery::decode_bmp_(uint8_t *d, size_t n) {
  if (n < 54)
    return false;
  const uint32_t off = rd32(d + 10);
  const uint32_t hdr = rd32(d + 14);
  int32_t w = (int32_t) rd32(d + 18);
  int32_t h = (int32_t) rd32(d + 22);
  const uint16_t bpp = rd16(d + 28);
  const uint32_t comp = rd32(d + 30);
  if (hdr < 40 || (bpp != 24 && bpp != 32) || (comp != 0 && comp != 3) || w <= 0 || h == 0) {
    ESP_LOGE(TAG, "Unsupported BMP (need uncompressed 24/32 bpp)");
    return false;
  }
  const bool topdown = h < 0;
  if (topdown)
    h = -h;
  const size_t bytes_pp = bpp / 8;
  const size_t stride = ((size_t) w * bytes_pp + 3) & ~(size_t) 3;

  this->src_w_ = w;
  this->src_h_ = h;
  this->src_ = (uint8_t *) psram_alloc((size_t) w * h * 3);
  if (this->src_ == nullptr) {
    ESP_LOGE(TAG, "Out of memory for %ldx%ld source", (long) w, (long) h);
    return false;
  }
  for (int y = 0; y < h; y++) {
    const size_t row_idx = topdown ? y : (h - 1 - y);
    const uint8_t *row = d + off + row_idx * stride;
    if (row + stride > d + n) {
      ESP_LOGW(TAG, "BMP truncated at row %d", y);
      break;
    }
    uint8_t *o = this->src_ + (size_t) y * w * 3;
    for (int x = 0; x < w; x++) {
      const uint8_t *p = row + x * bytes_pp;
      o[0] = p[2];
      o[1] = p[1];
      o[2] = p[0];
      o += 3;
    }
    if ((y & 31) == 0)
      feed_wdt_safe();
  }
  return true;
}

// ---------------------------------------------------------------- resample

void EPaperGallery::resample_() {
  const int tw = this->dst_w_;
  const int th = this->dst_h_;
  this->dst_ = (uint8_t *) psram_alloc((size_t) tw * th * 3);
  memset(this->dst_, 0xFF, (size_t) tw * th * 3);  // white background

  // Optionally rotate the source 90° CW so that portrait photos fill a
  // landscape display (and vice versa).
  const bool rotate = this->auto_rotate_ && ((this->src_w_ < this->src_h_) != (tw < th));
  const int sw = rotate ? this->src_h_ : this->src_w_;
  const int sh = rotate ? this->src_w_ : this->src_h_;
  const uint8_t *src = this->src_;
  const int src_w = this->src_w_, src_h = this->src_h_;
  auto sample = [&](int x, int y, int c) -> int {
    int sx = rotate ? y : x;
    int sy = rotate ? (src_h - 1 - x) : y;
    return src[((size_t) sy * src_w + sx) * 3 + c];
  };

  const float fx = (float) tw / sw;
  const float fy = (float) th / sh;
  const float s = this->scale_mode_ == SCALE_FILL ? std::max(fx, fy) : std::min(fx, fy);
  const int ow = (int) lroundf(sw * s);
  const int oh = (int) lroundf(sh * s);
  const int ox = (tw - ow) / 2;
  const int oy = (th - oh) / 2;

  for (int y = 0; y < th; y++) {
    const int iy = y - oy;
    if (iy < 0 || iy >= oh)
      continue;
    float syf = (iy + 0.5f) / s - 0.5f;
    int y0 = (int) floorf(syf);
    float wy = syf - y0;
    y0 = std::max(0, std::min(sh - 1, y0));
    int y1 = std::min(sh - 1, y0 + 1);

    uint8_t *o = this->dst_ + (size_t) y * tw * 3;
    for (int x = 0; x < tw; x++, o += 3) {
      const int ix = x - ox;
      if (ix < 0 || ix >= ow)
        continue;
      float sxf = (ix + 0.5f) / s - 0.5f;
      int x0 = (int) floorf(sxf);
      float wx = sxf - x0;
      x0 = std::max(0, std::min(sw - 1, x0));
      int x1 = std::min(sw - 1, x0 + 1);

      for (int c = 0; c < 3; c++) {
        float top = sample(x0, y0, c) * (1 - wx) + sample(x1, y0, c) * wx;
        float bot = sample(x0, y1, c) * (1 - wx) + sample(x1, y1, c) * wx;
        o[c] = (uint8_t) (top * (1 - wy) + bot * wy + 0.5f);
      }
    }
    if ((y & 15) == 0)
      feed_wdt_safe();
  }
}

// ---------------------------------------------------------------- dither

void EPaperGallery::dither_() {
  const int w = this->dst_w_;
  const int h = this->dst_h_;
  this->idx_ = (uint8_t *) psram_alloc((size_t) w * h);
  if (this->idx_ == nullptr) {
    ESP_LOGE(TAG, "Out of memory for index buffer");
    return;
  }
  this->prepare_palette_();

  // error buffers for the current and next row, index (x+1) to allow x-1/x+1 without bounds checks
  std::vector<int16_t> err_cur((w + 2) * 3, 0);
  std::vector<int16_t> err_next((w + 2) * 3, 0);

  for (int y = 0; y < h; y++) {
    const bool reverse = (y & 1) != 0;  // serpentine scan reduces visible patterns
    const int dir = reverse ? -1 : 1;

    for (int i = 0; i < w; i++) {
      const int x = reverse ? (w - 1 - i) : i;
      const uint8_t *p = this->dst_ + ((size_t) y * w + x) * 3;
      int16_t *ec = &err_cur[(x + 1) * 3];

      int v[3];
      for (int c = 0; c < 3; c++) {
        int t = p[c] + (this->dither_ ? ec[c] : 0);
        v[c] = t < 0 ? 0 : (t > 255 ? 255 : t);
      }

      const int best = this->nearest_(v);
      this->idx_[(size_t) y * w + x] = (uint8_t) best;

      if (!this->dither_)
        continue;

      // Floyd–Steinberg: 7/16 right, 3/16 below-left, 5/16 below, 1/16 below-right
      for (int c = 0; c < 3; c++) {
        int e = v[c] - this->match_rgb_[best][c];
        err_cur[(x + 1 + dir) * 3 + c] += (e * 7) / 16;
        err_next[(x + 1 - dir) * 3 + c] += (e * 3) / 16;
        err_next[(x + 1) * 3 + c] += (e * 5) / 16;
        err_next[(x + 1 + dir) * 3 + c] += (e * 1) / 16;
      }
    }
    std::swap(err_cur, err_next);
    std::fill(err_next.begin(), err_next.end(), 0);
    if ((y & 15) == 0)
      feed_wdt_safe();
  }
}

}  // namespace epaper_gallery
}  // namespace esphome
