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
2. **Verify the SD card pins** in `packages/photopainter.yaml` against the
   Waveshare Arduino example (`SD_MMC.setPins`) – they are placeholders.
3. Flash, then upload images to `http://photopainter.local/file/images/`.
4. Press *Refresh image list*, pick an image in the *Image* select.

## Component options

```yaml
epaper_gallery:
  display_id: epd
  root_path: /sdcard/images   # folder on the SD card
  scale: fit                  # fit | fill
  dither: true
  auto_rotate: true           # rotate portrait photos 90° to fill the landscape panel
```

Lambda API: `show("file.jpg")`, `show_next()`, `show_previous()`,
`refresh_list()`, `get_files()`, `get_current()`,
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
