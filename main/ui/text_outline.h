/*
 * Outlined text rendering.
 *
 * The card word is drawn as a white (or accent-coloured) fill inside a black
 * outline. On a 6-ink panel there is no anti-aliasing to soften glyph edges,
 * so a hard outline is what makes large text read cleanly against white.
 *
 * Auto-fit: M5GFX's built-in GFX fonts top out at 24pt, so a big word is
 * achieved with integer setTextSize() scaling. pickForBox() measures the
 * actual string with the real font metrics and returns the largest scale that
 * fits the box -- short words come out large, long words step down.
 */
#pragma once

#include <cstdint>
#include <M5GFX.h>

namespace ui::text {

struct FontPick {
    const lgfx::IFont* font{nullptr};
    // Fractional, because LovyanGFX's setTextSize() takes floats. DejaVu72 is
    // the largest native face available and is still short of filling the
    // word band, so short words are scaled above 1.0.
    float size{1.0f};
};

/// Largest (font, integer scale) whose rendering of `s` fits within the box.
FontPick pickForBox(M5GFX& g, const char* s, int16_t max_w, int16_t max_h);

/// Width in pixels that `s` would occupy under `pick`.
int16_t measure(M5GFX& g, const char* s, const FontPick& pick);

/// Plain (un-outlined) string, vertically centred on `cy`.
void draw(M5GFX& g, const char* s, int16_t x, int16_t cy, const FontPick& pick,
          uint32_t color, textdatum_t datum = textdatum_t::middle_left);

/// One string, centred at (cx, cy), filled and outlined.
void drawOutlined(M5GFX& g, const char* s, int16_t cx, int16_t cy,
                  const FontPick& pick, uint32_t fill, uint32_t outline,
                  int radius);

/// Outlined text with an arbitrary datum, so it can be left/right aligned.
void drawOutlinedAt(M5GFX& g, const char* s, int16_t x, int16_t cy,
                    const FontPick& pick, uint32_t fill, uint32_t outline,
                    int radius, textdatum_t datum);

/// A word split into prefix / accented span / suffix, centred as a whole.
/// `span_start` and `span_len` are byte offsets into `word` (ASCII only).
/// Outlines for every run are laid down before any fill, so neighbouring
/// outlines can never paint over an adjacent glyph's interior.
void drawWordWithAccent(M5GFX& g, const char* word, int span_start, int span_len,
                        int16_t cx, int16_t cy, const FontPick& pick,
                        uint32_t fill, uint32_t accent, uint32_t outline,
                        int radius);

}  // namespace ui::text
