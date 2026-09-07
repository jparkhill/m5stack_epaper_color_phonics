#!/usr/bin/env python3
"""
Generate the status-bar gradient that gets embedded in the firmware.

A six-ink panel cannot show a smooth gradient directly -- painting four flat
bands is the naive alternative and looks like four flat bands. Instead we
build a continuous gradient at full colour depth on the host, error-diffuse
it onto the panel's exact six inks, and embed the result as a PNG. Dithering
across the boundaries reads as a genuine sweep rather than hard blocks.

    tools/.venv/bin/python tools/gen_chrome.py

Writes main/embedded/statusbar.png (400x64).
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_assets import PALETTE, dither_to_palette_fast  # noqa: E402

WIDTH = 400
HEIGHT = 64
OUT = Path(__file__).resolve().parent.parent / "main" / "embedded" / "statusbar.png"

# Anchor colours for the sweep, left to right. Chosen to stay inside the hues
# the panel can actually hit (red/yellow/green/blue) so the dither has real
# ink to work with instead of trying to fake cyan or magenta.
STOPS = [
    (0.00, (196, 24, 24)),    # red
    (0.28, (232, 176, 32)),   # amber
    (0.52, (96, 150, 48)),    # green
    (0.76, (56, 110, 190)),   # blue
    (1.00, (120, 80, 210)),   # violet-ish
]


def gradient():
    """Horizontal multi-stop linear gradient, brightened toward the top."""
    xs = np.linspace(0.0, 1.0, WIDTH)
    row = np.zeros((WIDTH, 3), dtype=np.float64)
    for i in range(len(STOPS) - 1):
        t0, c0 = STOPS[i]
        t1, c1 = STOPS[i + 1]
        m = (xs >= t0) & (xs <= t1)
        if not m.any():
            continue
        f = ((xs[m] - t0) / (t1 - t0))[:, None]
        row[m] = np.array(c0)[None, :] * (1 - f) + np.array(c1)[None, :] * f

    img = np.repeat(row[None, :, :], HEIGHT, axis=0)

    # A subtle vertical lift stops the bar reading as a flat stripe and gives
    # the dither something to work with, which softens the banding further.
    ys = np.linspace(1.16, 0.90, HEIGHT)[:, None, None]
    img = np.clip(img * ys, 0, 255)
    return Image.fromarray(img.astype(np.uint8), "RGB")


def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    src = gradient()
    out = dither_to_palette_fast(src)

    arr = np.asarray(out).reshape(-1, 3)
    inks = {tuple(c) for c in np.unique(arr, axis=0)}
    allowed = {tuple(int(v) for v in p) for p in PALETTE}
    stray = inks - allowed
    if stray:
        raise SystemExit(f"off-palette pixels produced: {list(stray)[:4]}")

    out.save(OUT, "PNG", optimize=True)
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes, {len(inks)} inks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
