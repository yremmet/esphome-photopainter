# esphome-photopainter

ESPHome configuration and an external component for the
**Waveshare ESP32-S3-PhotoPainter** (7.3" Spectra E6 colour e-paper, 800×480).

What you get:

- **BLE proxy** for Home Assistant
- **SD card picture library** with upload/download/delete web UI (`/file/…`)
- **On-device image pipeline**: JPEG/BMP → scale (fit/fill, auto-rotate) →
  Floyd–Steinberg dithering to the 6-colour palette → display
- Home Assistant `select` entity to pick the image, buttons for next/previous/refresh
- Onboard SHTC3 temperature/humidity and PCF85063 RTC

## Layout

```
components/epaper_gallery/   external component (C++ + codegen)
packages/photopainter.yaml   full device config as ESPHome package
example/photopainter.yaml    minimal per-device YAML using the package
```

## Usage

1. Copy `example/photopainter.yaml` and `example/secrets.yaml.example` (as `secrets.yaml`)
   into your ESPHome config directory and fill in the secrets.
2. SD card: FAT32, create a folder `images/` on it.
3. Flash, then upload images to `http://photopainter.local/file/images/`.
4. Press *Refresh image list*, pick an image in the *Image* select.

## Component options

```yaml
epaper_gallery:
  display_id: epd
  root_path: /sdcard/images   # folder on the SD card
  scale: fit                  # fit | fill
  auto_rotate: true           # rotate portrait photos 90° to fill the landscape panel
  dither: floyd_steinberg     # none | floyd_steinberg | atkinson | jarvis_judice_ninke | dizzy
  error_clamp: 64             # 0..255, 255 = off
  dither_strength: 100%
  tone_compression: 100%      # squeeze 0..255 into the panel's black..white range
  tone_curve: linear          # linear | soft (soft keeps midtone contrast)
  saturation: 1.3             # boost before dithering, 1.0 = unchanged
  page_id: photo_page         # optional, see below
```

The gallery is a drawing source like any other: it keeps the last rendered image
and paints it from a display lambda or page. A finished image triggers `update()`
on the display (and switches to `page_id` first, if set).

```yaml
display:
  - platform: epaper_spi
    id: epd
    # ...
    pages:
      - id: photo_page
        lambda: id(gallery).draw(it);
      - id: dashboard_page
        lambda: |-
          it.fill(Color::WHITE);
          // your own stuff
```

Lambda API: `show("file.jpg")`, `show_next()`, `show_previous()`,
`refresh_list()`, `get_files()`, `get_current()`, `draw(it)`,
`show_calibration_pattern()`, `set_palette_preset("aitjcize")`,
`set_dither_method(...)`, `set_error_clamp(n)`, `set_dither_strength(f)`,
`set_tone_compression(f)`, `set_tone_curve(...)`, `set_saturation(f)`,
`add_on_list_changed_callback(cb)`, `add_on_image_shown_callback(cb)`.

## Notes

- Requires ESP-IDF framework and PSRAM (octal).
- Progressive JPEGs are not supported by JPEGDEC – export baseline.
- `MATCH_PALETTE` in `epaper_gallery.cpp` approximates the real pigment colours
  and is the main knob for photo quality.
- PMIC (AXP2101) init sequence based on
  [esphome discussion #3790](https://github.com/orgs/esphome/discussions/3790).
- SD card support via [n-serrette/esphome_sd_card](https://github.com/n-serrette/esphome_sd_card).

## License

MIT

## Vendored components

`components/sd_mmc_card` and `components/sd_file_server` are copied from
[n-serrette/esphome_sd_card](https://github.com/n-serrette/esphome_sd_card)
(see `LICENSE.upstream`) and patched for the ESPHome ≥ 2026 `web_server_idf`
API (`url_to()` / removed `network::get_use_address()`).
