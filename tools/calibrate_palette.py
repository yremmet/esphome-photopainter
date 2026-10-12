#!/usr/bin/env python3
"""Derive the epaper_gallery `palette:` block from a photo of the calibration pattern.

1. Press "Show calibration pattern" on the device (3x2 patches: black white yellow / red blue green).
2. Photograph the panel in daylight, straight on, no reflections. Crop the photo so that
   ONLY the panel is visible (any image editor), save as calib.jpg.
3. python3 calibrate_palette.py calib.jpg

The script samples the centre of each patch, white-balances on the white patch and prints
a YAML snippet. Paste it under `epaper_gallery:`.
"""
import sys
import numpy as np
from PIL import Image, ImageOps

NAMES = ["black", "white", "yellow", "red", "blue", "green"]

if len(sys.argv) < 2:
    sys.exit(__doc__)
im = np.asarray(ImageOps.exif_transpose(Image.open(sys.argv[1])).convert("RGB")).astype(float)
h, w, _ = im.shape
pw, ph = w / 3, h / 2

samples = []
for i in range(6):
    cx, cy = (i % 3 + 0.5) * pw, (i // 3 + 0.5) * ph
    # central 40 % of the patch, median to ignore glare/noise
    x0, x1 = int(cx - pw * 0.2), int(cx + pw * 0.2)
    y0, y1 = int(cy - ph * 0.2), int(cy + ph * 0.2)
    samples.append(np.median(im[y0:y1, x0:x1].reshape(-1, 3), axis=0))
samples = np.array(samples)

# white balance: make the white patch neutral, keep its brightness
white = samples[1]
gain = white.mean() / white
samples = np.clip(samples * gain, 0, 255)

print("epaper_gallery:")
print("  color_space: lab")
print("  palette:")
for name, rgb in zip(NAMES, samples):
    r, g, b = (int(round(v)) for v in rgb)
    print(f'    {name}: "#{r:02X}{g:02X}{b:02X}"')
