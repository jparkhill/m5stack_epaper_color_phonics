#include "ui/text_outline.h"

#include <cstring>
#include <algorithm>

namespace ui::text {
namespace {

// Ordered largest-first. Integer scaling of a bitmap font is chunky, which
// suits a children's flashcard, but we still prefer a bigger base font over a
// scaled smaller one at equal height.
struct Candidate {
    const lgfx::IFont* font;
    float size;
};

// Ordered by rendered height, largest first. pickForBox() walks this and
// takes the first entry that actually fits the box, measured with real font
// metrics -- so a short word like "cat" gets the 72px face while
// "jellyfish" steps down rather than overflowing.
//
// M5GFX's FreeSans GFX fonts stop at 24pt, which is only ~34px tall and looks
// lost in a 118px band. The DejaVu faces go up to 72px natively, which is why
// they lead here: native glyphs at 72px are far cleaner than a 24pt bitmap
// scaled 2x.
// Ordered by rendered height, largest first. pickForBox() takes the first
// entry that actually fits, measured with real font metrics, so short words
// come out huge and long ones step down -- always filling the band without
// ever escaping it.
//
// DejaVu72 is the biggest native face M5GFX ships and is still only ~72px in
// a 118px band, so it is also offered scaled up. Nearest-neighbour scaling of
// a bitmap face is chunky, but with a heavy black outline on a six-ink panel
// that reads as a deliberate sticker-book look rather than an artefact.
const Candidate kCandidates[] = {
    {&fonts::DejaVu72, 1.55f},
    {&fonts::DejaVu72, 1.40f},
    {&fonts::DejaVu72, 1.25f},
    {&fonts::DejaVu72, 1.10f},
    {&fonts::DejaVu72, 1.00f},
    {&fonts::DejaVu56, 1.20f},
    {&fonts::DejaVu56, 1.00f},
    {&fonts::DejaVu40, 1.20f},
    {&fonts::DejaVu40, 1.00f},
    {&fonts::FreeSansBold24pt7b, 1.00f},
    {&fonts::DejaVu24, 1.00f},
};

constexpr int kMaxRuns = 3;

struct Run {
    char text[32];
    uint32_t color;
    int16_t width;
};

void applyPick(M5GFX& g, const FontPick& pick) {
    if (pick.font != nullptr) g.setFont(pick.font);
    g.setTextSize(pick.size);
}

/// Copy a byte range out of `src` into a NUL-terminated buffer.
void slice(char* dst, size_t dst_size, const char* src, int start, int len) {
    if (len < 0) len = 0;
    if (static_cast<size_t>(len) >= dst_size) len = static_cast<int>(dst_size) - 1;
    std::memcpy(dst, src + start, static_cast<size_t>(len));
    dst[len] = '\0';
}

}  // namespace

FontPick pickForBox(M5GFX& g, const char* s, int16_t max_w, int16_t max_h) {
    if (s == nullptr || *s == '\0') return FontPick{&fonts::FreeSansBold12pt7b, 1.0f};

    for (const auto& c : kCandidates) {
        g.setFont(c.font);
        g.setTextSize(c.size);
        const int16_t w = static_cast<int16_t>(g.textWidth(s));
        const int16_t h = static_cast<int16_t>(g.fontHeight());
        if (w <= max_w && h <= max_h) {
            return FontPick{c.font, c.size};
        }
    }
    // Nothing fits; return the smallest so the caller still renders something
    // legible rather than nothing at all.
    return FontPick{&fonts::FreeSansBold9pt7b, 1.0f};
}

int16_t measure(M5GFX& g, const char* s, const FontPick& pick) {
    if (s == nullptr) return 0;
    applyPick(g, pick);
    return static_cast<int16_t>(g.textWidth(s));
}

void draw(M5GFX& g, const char* s, int16_t x, int16_t cy, const FontPick& pick,
          uint32_t color, textdatum_t datum) {
    if (s == nullptr || *s == '\0') return;
    applyPick(g, pick);
    g.setTextDatum(datum);
    g.setTextColor(color);
    g.drawString(s, x, cy);
}

void drawOutlined(M5GFX& g, const char* s, int16_t cx, int16_t cy,
                  const FontPick& pick, uint32_t fill, uint32_t outline,
                  int radius) {
    if (s == nullptr || *s == '\0') return;
    applyPick(g, pick);
    g.setTextDatum(textdatum_t::middle_center);

    g.setTextColor(outline);
    const int r2 = radius * radius;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx == 0 && dy == 0) continue;
            if (dx * dx + dy * dy > r2) continue;
            g.drawString(s, cx + dx, cy + dy);
        }
    }

    g.setTextColor(fill);
    g.drawString(s, cx, cy);
}

void drawOutlinedAt(M5GFX& g, const char* s, int16_t x, int16_t cy,
                    const FontPick& pick, uint32_t fill, uint32_t outline,
                    int radius, textdatum_t datum) {
    if (s == nullptr || *s == '\0') return;
    applyPick(g, pick);
    g.setTextDatum(datum);

    g.setTextColor(outline);
    const int r2 = radius * radius;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx == 0 && dy == 0) continue;
            if (dx * dx + dy * dy > r2) continue;
            g.drawString(s, x + dx, cy + dy);
        }
    }

    g.setTextColor(fill);
    g.drawString(s, x, cy);
}

void drawWordWithAccent(M5GFX& g, const char* word, int span_start, int span_len,
                        int16_t cx, int16_t cy, const FontPick& pick,
                        uint32_t fill, uint32_t accent, uint32_t outline,
                        int radius) {
    if (word == nullptr || *word == '\0') return;

    const int total_len = static_cast<int>(std::strlen(word));
    span_start = std::clamp(span_start, 0, total_len);
    span_len = std::clamp(span_len, 0, total_len - span_start);

    // If there is no accented span, this degenerates to a single centred run.
    if (span_len == 0) {
        drawOutlined(g, word, cx, cy, pick, fill, outline, radius);
        return;
    }

    Run runs[kMaxRuns];
    int n = 0;

    if (span_start > 0) {
        slice(runs[n].text, sizeof(runs[n].text), word, 0, span_start);
        runs[n].color = fill;
        ++n;
    }
    slice(runs[n].text, sizeof(runs[n].text), word, span_start, span_len);
    runs[n].color = accent;
    ++n;
    const int tail_start = span_start + span_len;
    if (tail_start < total_len) {
        slice(runs[n].text, sizeof(runs[n].text), word, tail_start,
              total_len - tail_start);
        runs[n].color = fill;
        ++n;
    }

    applyPick(g, pick);
    g.setTextDatum(textdatum_t::middle_left);

    int16_t total_w = 0;
    for (int i = 0; i < n; ++i) {
        runs[i].width = static_cast<int16_t>(g.textWidth(runs[i].text));
        total_w = static_cast<int16_t>(total_w + runs[i].width);
    }

    const int16_t x0 = static_cast<int16_t>(cx - total_w / 2);

    // Pass 1: every outline. Doing all outlines before any fill stops one
    // run's halo from eating into the neighbouring glyph's interior.
    g.setTextColor(outline);
    const int r2 = radius * radius;
    int16_t x = x0;
    for (int i = 0; i < n; ++i) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx == 0 && dy == 0) continue;
                if (dx * dx + dy * dy > r2) continue;
                g.drawString(runs[i].text, x + dx, cy + dy);
            }
        }
        x = static_cast<int16_t>(x + runs[i].width);
    }

    // Pass 2: fills.
    x = x0;
    for (int i = 0; i < n; ++i) {
        g.setTextColor(runs[i].color);
        g.drawString(runs[i].text, x, cy);
        x = static_cast<int16_t>(x + runs[i].width);
    }
}

}  // namespace ui::text
