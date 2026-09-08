#!/usr/bin/env python3
"""
Render the device screen on the host, so layout and typography can be judged
without waiting ~16 s for a real e-paper refresh.

    tools/.venv/bin/python tools/preview_layout.py --card p_pizza -o /tmp/p.png

Geometry is PARSED OUT OF main/ui/theme.h rather than duplicated here, so the
preview cannot silently drift from the firmware. Fonts are DejaVu at matching
pixel heights (the firmware uses M5GFX's DejaVu faces), and the result is
dithered to the panel's six inks -- so what you see is close to what the panel
will actually show, including how the dither treats the word outline.

This is an approximation of glyph metrics, not a pixel-exact simulator: use it
for proportions, sizing and spacing decisions.
"""

import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_assets import PALETTE, dither_to_palette_fast  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
THEME = ROOT / "main" / "ui" / "theme.h"
ASSETS = ROOT / "assets" / "phonics"
STATUSBAR_PNG = ROOT / "main" / "embedded" / "statusbar.png"

FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONT_REG = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

# Mirrors the firmware's candidate ladder in ui/text_outline.cpp: DejaVu72 at
# a set of scales, then smaller faces. Values are pixel heights.
WORD_LADDER = [int(72 * s) for s in (1.55, 1.40, 1.25, 1.10, 1.00)] + [67, 56, 48, 40, 34, 24]


def parse_theme():
    """Pull `constexpr int16_t kFoo = 123;` out of theme.h."""
    text = THEME.read_text()
    out = {}
    for m in re.finditer(r"constexpr\s+(?:int16_t|int)\s+(k\w+)\s*=\s*(-?\d+)\s*;", text):
        out[m.group(1)] = int(m.group(2))
    for m in re.finditer(r"constexpr\s+uint32_t\s+(k\w+)\s*=\s*0x([0-9A-Fa-f]+)u?\s*;", text):
        out[m.group(1)] = int(m.group(2), 16)
    for m in re.finditer(r"constexpr\s+bool\s+(k\w+)\s*=\s*(true|false)\s*;", text):
        out[m.group(1)] = (m.group(2) == "true")

    # The semantic colour roles are aliases of the palette entries
    # (kAccentRule = kBlue), so resolve name -> name references. Repeat a few
    # times so an alias of an alias also settles.
    aliases = dict(re.findall(
        r"constexpr\s+(?:uint32_t|bool|int16_t|int)\s+(k\w+)\s*=\s*(k\w+)\s*;", text))
    for _ in range(4):
        for name, target in aliases.items():
            if name not in out and target in out:
                out[name] = out[target]
    missing = [n for n in aliases if n not in out]
    if missing:
        raise SystemExit(f"theme.h aliases could not be resolved: {missing}")
    return out


def rgb(v):
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def fit_word(word, max_w, max_h):
    """Largest ladder entry whose rendering fits the box, as the firmware does."""
    for px in WORD_LADDER:
        f = ImageFont.truetype(FONT_BOLD, px)
        box = f.getbbox(word)
        if (box[2] - box[0]) <= max_w and (box[3] - box[1]) <= max_h:
            return f, px
    return ImageFont.truetype(FONT_BOLD, WORD_LADDER[-1]), WORD_LADDER[-1]


def draw_outlined_runs(draw, runs, cx, cy, font, outline, radius):
    """Two passes: every outline first, then every fill.

    Matches the firmware: doing all outlines before any fill stops one run's
    halo from eating into an adjacent glyph's interior.
    """
    widths = [draw.textlength(t, font=font) for t, _ in runs]
    total = sum(widths)
    x = cx - total / 2

    ascent, descent = font.getmetrics()
    baseline = cy + (ascent - (ascent + descent) / 2)

    xs = []
    cursor = x
    for w in widths:
        xs.append(cursor)
        cursor += w

    for (text, _), x0 in zip(runs, xs):
        for dy in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                if dx == 0 and dy == 0:
                    continue
                if dx * dx + dy * dy > radius * radius:
                    continue
                draw.text((x0 + dx, baseline + dy), text, font=font,
                          fill=outline, anchor="ls")
    for (text, colour), x0 in zip(runs, xs):
        draw.text((x0, baseline), text, font=font, fill=colour, anchor="ls")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--card", default=None, help="card id, e.g. p_pizza")
    ap.add_argument("-o", "--out", default="/tmp/phonics_preview.png")
    ap.add_argument("--time", default="16:45")
    ap.add_argument("--date", default="Sun 7 Sep")
    ap.add_argument("--temp", default="22.4")
    ap.add_argument("--humidity", default="41")
    ap.add_argument("--no-dither", action="store_true")
    args = ap.parse_args()

    t = parse_theme()
    W, H = t["kScreenW"], t["kScreenH"]

    manifest = json.loads((ASSETS / "manifest.json").read_text())
    cards = {c["id"]: c for c in manifest["cards"]}
    card = cards.get(args.card) or manifest["cards"][0]

    img = Image.new("RGB", (W, H), rgb(t["kWhite"]))
    draw = ImageDraw.Draw(img)

    # --- status bar: blit the same pre-dithered gradient the firmware embeds
    if t.get("kStatusRainbow") and STATUSBAR_PNG.exists():
        bar = Image.open(STATUSBAR_PNG).convert("RGB").resize(
            (t["kStatusW"], t["kStatusH"]), Image.NEAREST)
        img.paste(bar, (t["kStatusX"], t["kStatusY"]))

    f_time = ImageFont.truetype(FONT_BOLD, 34)
    f_small = ImageFont.truetype(FONT_BOLD, 22)
    white, black = rgb(t["kWhite"]), rgb(t["kBlack"])

    def bar_text(s, x, cy, font, anchor):
        for dy in (-2, -1, 0, 1, 2):
            for dx in (-2, -1, 0, 1, 2):
                if dx or dy:
                    draw.text((x + dx, cy + dy), s, font=font, fill=black, anchor=anchor)
        draw.text((x, cy), s, font=font, fill=white, anchor=anchor)

    bar_text(args.time, t["kStatusX"] + 10, t["kStatusY"] + 20, f_time, "lm")
    bar_text(args.date, t["kStatusX"] + 10, t["kStatusY"] + 47, f_small, "lm")
    right = t["kStatusX"] + t["kStatusW"] - 10
    bar_text(f"{args.temp} C", right, t["kStatusY"] + 20, f_time, "rm")
    bar_text(f"{args.humidity}% RH", right, t["kStatusY"] + 47, f_small, "rm")

    # --- accent rule
    draw.rectangle([0, t["kRuleY"], W, t["kRuleY"] + t["kRuleH"] - 1],
                   fill=rgb(t["kAccentRule"]))

    # --- square image area + frame
    ix, iy, iw, ih = t["kImageX"], t["kImageY"], t["kImageW"], t["kImageH"]
    pic_path = ASSETS / card["image"]
    if pic_path.exists():
        pic = Image.open(pic_path).convert("RGB").resize((iw, ih), Image.NEAREST)
        img.paste(pic, (ix, iy))
    for i in range(t["kFrameThickness"]):
        draw.rectangle([ix - i, iy - i, ix + iw - 1 + i, iy + ih - 1 + i],
                       outline=rgb(t["kFrame"]))

    # --- the word
    wx, wy, ww, wh = t["kWordX"], t["kWordY"], t["kWordW"], t["kWordH"]
    radius = t["kWordOutlineRadius"]
    margin = t.get("kWordSideMargin", 6)
    avail_w = ww - 2 * (radius + margin)
    avail_h = wh - 2 * radius

    display = card["display"]
    start, length = card["span"]
    font, px = fit_word(display, avail_w, avail_h)

    accent = rgb(t["kGraphemeFill"]) if t.get("kAccentGrapheme") else rgb(t["kWordFill"])
    runs = []
    if start > 0:
        runs.append((display[:start], rgb(t["kWordFill"])))
    runs.append((display[start:start + length], accent))
    if start + length < len(display):
        runs.append((display[start + length:], rgb(t["kWordFill"])))

    draw_outlined_runs(draw, runs, wx + ww // 2, wy + wh // 2, font,
                       rgb(t["kWordOutline"]), radius)

    # --- button hints with arrows (no footer any more) ---
    f_hint = ImageFont.truetype(FONT_BOLD, 15)
    ink = rgb(t["kInk"])
    arrow = t.get("kArrowSize", 7)

    def tri_up(cx, cy):
        draw.polygon([(cx, cy - arrow), (cx - arrow, cy + arrow),
                      (cx + arrow, cy + arrow)], fill=ink)

    def tri_left(cx, cy):
        draw.polygon([(cx - arrow, cy), (cx + arrow, cy - arrow),
                      (cx + arrow, cy + arrow)], fill=ink)

    # top button: arrow points up into the middle of the title bar
    cy = t["kHintTopY"] + t["kHintTopH"] // 2
    label = "letter"
    lw = draw.textlength(label, font=f_hint)
    total = lw + 2 * arrow + 6
    x0 = (W - total) / 2
    tri_up(x0 + arrow, cy)
    draw.text((x0 + 2 * arrow + 6, cy), label, font=f_hint, fill=ink, anchor="lm")

    # side buttons: arrows point left at each button's own height
    for text_, y in (("word", t["kHintSide1Y"] + t["kHintRowH"] // 2),
                     ("again", t["kHintSide2Y"] + t["kHintRowH"] // 2)):
        tri_left(t["kGutterX"] + 2 + arrow, y)
        draw.text((t["kGutterX"] + 2 * arrow + 6, y), text_, font=f_hint,
                  fill=ink, anchor="lm")

    if not args.no_dither:
        img = dither_to_palette_fast(img)

    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    img.save(args.out)

    print(f"card      : {card['id']}  display={display!r} span={card['span']}")
    print(f"word font : {px}px  (box {avail_w}x{avail_h})")
    print(f"regions   : status {t['kStatusW']}x{t['kStatusH']} @{t['kStatusX']},{t['kStatusY']}")
    print(f"            image  {iw}x{ih} @{ix},{iy}")
    print(f"            word   {ww}x{wh} @{wx},{wy}")
    print(f"            gutter {t['kGutterW']}px wide for the side hints")
    print(f"wrote     : {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
