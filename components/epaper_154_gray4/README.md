# epaper_154_gray4

ESPHome display platform for the **Waveshare 1.54" V2** e-paper (200×200, SSD1681, same glass as
Good Display GDEH0154D67 / GDEY0154D67) with **4 gray levels**: white, light gray, dark gray, black.
Built for the Waveshare ESP32-S3-ePaper-1.54 board, ESP-IDF framework.

- **Full update** = 4-gray refresh with a custom LUT (from GxEPD2_4G, see below)
- **Partial update** = no full-panel flash, in two steps: a black/white refresh with the panel's OTP
  waveform (the same one `waveshare_epaper` `1.54inv2` uses) for the pixels that change, then a short
  gray pulse only for pixels that become gray (see *Gray partial* below)
- 2-bit framebuffer (10 kB, PSRAM if available) plus a 10 kB copy of what the panel shows
- The refresh does not block: `update()` sends the image (~0.1 s) and returns, BUSY is polled in `loop()`,
  then the panel goes into deep sleep, like `waveshare_epaper` does

## Setup

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/yremmet/esphome-photopainter
      ref: main
    components: [epaper_154_gray4]

spi:
  clk_pin: GPIO12
  mosi_pin: GPIO13

display:
  - platform: epaper_154_gray4
    id: my_display
    cs_pin: GPIO11
    dc_pin: GPIO10
    busy_pin: GPIO8
    reset_pin: GPIO9
    reset_duration: 2ms
    rotation: 90
    update_interval: never
    full_update_every: 1
    pages: ...   # or lambda:
```

A complete minimal config is in [`example.yaml`](example.yaml).

| Option | Default | |
|---|---|---|
| `cs_pin` | – | required (SPI device) |
| `dc_pin` | – | required |
| `busy_pin` | – | required (the refresh is awaited via BUSY) |
| `reset_pin` | – | required (wakes the panel from deep sleep) |
| `reset_duration` | `200ms` | reset pulse, max 500 ms |
| `full_update_every` | `1` | every Nth update is a 4-gray refresh, the rest are B/W partial. `1` = always gray |
| `cleanup_passes` | `1` | partial refreshes of the first frame after a gray refresh, 1..5 (see below) |
| `rotation`, `pages`, `lambda`, `update_interval`, `auto_clear_enabled`, `data_rate` | | as for every ESPHome SPI display |

### Action: `epaper_154_gray4.full_update_next`

Make the next update a 4-gray full refresh, e.g. after a series of partial updates:

```yaml
- epaper_154_gray4.full_update_next: my_display
- component.update: my_display
```

`id(my_display).is_refreshing()` is true while the panel is still busy after `update()` returned.

## Colours

Colours are **ink**, like the binary ESPHome e-paper drivers: `COLOR_ON` = black, `COLOR_OFF` = white, so
existing lambdas run unchanged. Any other colour is mapped by luminance (Rec. 601) to the nearest level:
bright = black, `Color(170,170,170)` = dark gray, `Color(85,85,85)` = light gray, dark = white.

- **Fonts with `bpp: 2`** render anti-aliased (the edge pixels are blended between colour and background
  and land on the two gray levels). `bpp: 1` fonts work as before.
- **`image` with `type: GRAYSCALE`**: because of the ink convention, add `invert_alpha: true` (inverts the
  stored gray values) or the image shows as a negative.

## Limitations

- Gray partial (experimental): step 2 uses only group 2 of the 4-gray LUT (white -> light gray 3 frames,
  -> dark gray 8 frames), without the white-reset of groups 0-1 that makes the full refresh flash. The
  grays start from the white of a B/W partial instead of that reset, so they can come out slightly
  different from a full refresh and drift over many updates; a full update (`full_update_every`,
  `full_update_next`) resets them.
- A pixel that is gray and changes is reported to the differential waveform as the opposite of its new
  colour, so it is driven fully to black/white instead of being left as a ghost. `cleanup_passes: 2` (or
  more) repeats that step for less ghosting, ~0.3 s per pass.
- The 4 gray levels depend on the panel and on temperature (the LUT has no temperature compensation).
  Expect some ghosting from gray refreshes; a B/W partial refresh also does not clean up gray areas.
- Refresh time: partial ~0.3 s, 4-gray noticeably longer than the old B/W full refresh (~2 s) – measure it,
  the log prints `Gray refresh took … ms`.
- Since `update()` no longer blocks for the refresh, an automation that cuts the panel's power right after
  `component.update` (e.g. battery off) must wait first:
  `wait_until: { condition: { lambda: 'return !id(my_display).is_refreshing();' }, timeout: 5s }`.

## Sources

- **Gray LUT and 4-gray register sequence**: [GxEPD2_4G](https://github.com/ZinggJM/GxEPD2_4G),
  `src/gdey/GxEPD2_154_GDEY0154D67.cpp` (`lut_4G`, `_Init_4G`, `writeImage_4G`, `_Update_4G`), commit
  `de00616`; GxEPD2_4G takes the waveform from Good Display's demo. Good Display does not publish its
  4-gray demo for this panel ("If you need grayscale code, please contact us").
  Only bytes 0..152 are sent with `0x32`, as GxEPD2_4G does for this panel.
- **0x22 = 0xC7** for the gray refresh: SSD1681 datasheet rev 0.13, command `0x22`: `C7` = clock + analog
  on, *Display with DISPLAY Mode 1*, analog + OSC off – no temperature/LUT load from OTP, so the LUT written
  with `0x32` is used. `0xCF` is the same with Display Mode 2 (the differential mode for partial updates).
  GxEPD2_4G uses `0xC7`.
- **RAM → LUT**: LUT index = R-RAM bit × 2 + B/W-RAM bit (datasheet table 6-4/6-5); the LUT orders
  L0..L3 as white, light gray, dark gray, black, so the 2-bit level goes 1:1 into the two planes
  (bit 1 → `0x26`, bit 0 → `0x24`).
- **B/W partial update**: ESPHome `waveshare_epaper` `WaveshareEPaperTypeA` (`1.54inv2`): init sequence,
  RAM window, `0x22 = 0xFF`. Not taken over: the 30-byte LUT write (`0xFF` reloads the OTP LUT anyway).
  Added: the previous frame is written to `0x26`, as ESPHome's `epaper_spi` SSD1677 driver does, because
  after a gray refresh `0x26` holds a gray plane instead of the old image.
