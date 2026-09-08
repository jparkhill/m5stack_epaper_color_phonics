#include "ui/screen.h"
#include "ui/text_outline.h"
#include "ui/theme.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>

namespace ui {
namespace {
constexpr const char* kTag = "screen";

// Deliberately terse: this is 9pt text in a 66px gutter, and a child cannot
// read a sentence anyway -- the arrows carry most of the meaning.
constexpr const char* kHintTop   = "letter";
constexpr const char* kHintSide1 = "word";
constexpr const char* kHintSide2 = "again";
}

void Screen::init() {
    status_.setBounds(Rect{theme::kStatusX, theme::kStatusY, theme::kStatusW,
                           theme::kStatusH});

    card_.setBounds(Rect{theme::kImageX, theme::kImageY, theme::kImageW,
                         theme::kImageH});
    card_.setWordBounds(Rect{theme::kWordX, theme::kWordY, theme::kWordW,
                             theme::kWordH});

    widgets_[0] = &status_;
    widgets_[1] = &card_;
    widget_count_ = 2;


    ESP_LOGI(kTag, "layout: status %dx%d @%d,%d | image %dx%d @%d,%d | word %dx%d @%d,%d",
             theme::kStatusW, theme::kStatusH, theme::kStatusX, theme::kStatusY,
             theme::kImageW, theme::kImageH, theme::kImageX, theme::kImageY,
             theme::kWordW, theme::kWordH, theme::kWordX, theme::kWordY);
}

/// Small solid triangles, because the GFX fonts are ASCII-only and have no
/// arrow glyphs. Drawing them is both cheaper and sharper than shipping a
/// font just for this.
void Screen::drawArrowUp(M5GFX& g, int16_t cx, int16_t cy, int16_t size,
                         uint32_t color) {
    g.fillTriangle(cx, static_cast<int16_t>(cy - size),
                   static_cast<int16_t>(cx - size), static_cast<int16_t>(cy + size),
                   static_cast<int16_t>(cx + size), static_cast<int16_t>(cy + size),
                   color);
}

void Screen::drawArrowLeft(M5GFX& g, int16_t cx, int16_t cy, int16_t size,
                           uint32_t color) {
    g.fillTriangle(static_cast<int16_t>(cx - size), cy,
                   static_cast<int16_t>(cx + size), static_cast<int16_t>(cy - size),
                   static_cast<int16_t>(cx + size), static_cast<int16_t>(cy + size),
                   color);
}

void Screen::drawChrome(M5GFX& g) {
    // Accent rule separating the status bar from the card area.
    g.fillRect(0, theme::kRuleY, theme::kScreenW, theme::kRuleH,
               theme::kAccentRule);

    const text::FontPick tiny{&fonts::FreeSansBold9pt7b, 1.0f};

    // --- TOP button: arrow points up into the middle of the status bar ---
    {
        const int16_t cy =
            static_cast<int16_t>(theme::kHintTopY + theme::kHintTopH / 2);
        const int16_t label_w = text::measure(g, kHintTop, tiny);
        const int16_t total = static_cast<int16_t>(label_w + 2 * theme::kArrowSize + 6);
        const int16_t x0 = static_cast<int16_t>((theme::kScreenW - total) / 2);
        drawArrowUp(g, static_cast<int16_t>(x0 + theme::kArrowSize), cy,
                    theme::kArrowSize, theme::kHintInk);
        text::draw(g, kHintTop,
                   static_cast<int16_t>(x0 + 2 * theme::kArrowSize + 6), cy, tiny,
                   theme::kHintInk, textdatum_t::middle_left);
    }

    // --- SIDE buttons: arrows point left, at the buttons' own heights ---
    const struct { const char* text; int16_t y; } side[] = {
        {kHintSide1, static_cast<int16_t>(theme::kHintSide1Y + theme::kHintRowH / 2)},
        {kHintSide2, static_cast<int16_t>(theme::kHintSide2Y + theme::kHintRowH / 2)},
    };
    for (const auto& row : side) {
        drawArrowLeft(g, static_cast<int16_t>(theme::kGutterX + 2 + theme::kArrowSize),
                      row.y, theme::kArrowSize, theme::kHintInk);
        text::draw(g, row.text,
                   static_cast<int16_t>(theme::kGutterX + 2 * theme::kArrowSize + 6),
                   row.y, tiny, theme::kHintInk, textdatum_t::middle_left);
    }
}

void Screen::render() {
    auto& g = hal::display::gfx();
    ESP_LOGD(kTag, "render: clear");
    hal::display::beginFrame();
    ESP_LOGD(kTag, "render: chrome");
    drawChrome(g);
    for (size_t i = 0; i < widget_count_; ++i) {
        if (widgets_[i] == nullptr) continue;
        ESP_LOGD(kTag, "render: widget %s", widgets_[i]->name());
        widgets_[i]->draw(g);
    }
    ESP_LOGD(kTag, "render: done");
}

uint32_t Screen::present(hal::display::RefreshMode mode) {
    render();
    return hal::display::present(mode);
}

void Screen::describe() const {
    ESP_LOGI(kTag, "%u widget region(s):", (unsigned)widget_count_);
    for (size_t i = 0; i < widget_count_; ++i) {
        if (widgets_[i] == nullptr) continue;
        const Rect& r = widgets_[i]->bounds();
        ESP_LOGI(kTag, "  %-12s x=%3d y=%3d w=%3d h=%3d", widgets_[i]->name(), r.x,
                 r.y, r.w, r.h);
    }
    ESP_LOGI(kTag, "  hints: top=\"%s\" side1=\"%s\" side2=\"%s\"", kHintTop,
             kHintSide1, kHintSide2);
}

}  // namespace ui
