#include "ui/screen.h"
#include "ui/text_outline.h"
#include "ui/theme.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>

namespace ui {
namespace {
constexpr const char* kTag = "screen";
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

    setFooterHint("Side: new word    Top: next letter");
    ESP_LOGI(kTag, "layout: status %dx%d @%d,%d | image %dx%d @%d,%d | word %dx%d @%d,%d",
             theme::kStatusW, theme::kStatusH, theme::kStatusX, theme::kStatusY,
             theme::kImageW, theme::kImageH, theme::kImageX, theme::kImageY,
             theme::kWordW, theme::kWordH, theme::kWordX, theme::kWordY);
}

void Screen::setFooterHint(const char* hint) {
    std::snprintf(footer_, sizeof(footer_), "%s", hint ? hint : "");
}

void Screen::drawChrome(M5GFX& g) {
    // Accent rule separating the status bar from the card area.
    g.fillRect(0, theme::kRuleY, theme::kScreenW, theme::kRuleH, theme::kAccentRule);

    if (footer_[0] != '\0') {
        const text::FontPick small{&fonts::FreeSansBold9pt7b, 1};
        text::draw(g, footer_, static_cast<int16_t>(theme::kScreenW / 2),
                   static_cast<int16_t>(theme::kFooterY + theme::kFooterH / 2),
                   small, theme::kInk, textdatum_t::middle_center);
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
    ESP_LOGI(kTag, "  footer: \"%s\"", footer_);
}

}  // namespace ui
