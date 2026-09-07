#include "ui/w_cardview.h"
#include "ui/text_outline.h"
#include "ui/theme.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>

namespace ui {
namespace {
constexpr const char* kTag = "cardview";
}

void CardView::setPlaceholder(const char* line1, const char* line2) {
    std::snprintf(ph1_, sizeof(ph1_), "%s", line1 ? line1 : "");
    std::snprintf(ph2_, sizeof(ph2_), "%s", line2 ? line2 : "");
}

void CardView::draw(M5GFX& g) {
    // Frame around the square picture area. Drawn regardless so the layout
    // reads as intentional even with no card loaded.
    for (int i = 0; i < theme::kFrameThickness; ++i) {
        g.drawRect(static_cast<int16_t>(bounds_.x - i),
                   static_cast<int16_t>(bounds_.y - i),
                   static_cast<int16_t>(bounds_.w + 2 * i),
                   static_cast<int16_t>(bounds_.h + 2 * i), theme::kFrame);
    }

    if (card_ == nullptr) {
        drawPlaceholder(g);
        return;
    }
    drawImage(g);
    drawWord(g);
}

void CardView::drawImage(M5GFX& g) {
    ESP_LOGD(kTag, "drawImage: %s (%s, %u bytes)", card_->id,
             card_->isBuiltin() ? "flash" : "sd",
             card_->isBuiltin() ? card_->image_len : 0u);
    // Images are pre-dithered on the host to the panel's exact six inks and
    // pre-scaled to fit this box, so no on-device scaling or dithering.
    const bool ok =
        card_->isBuiltin()
            ? g.drawPng(card_->image_data, card_->image_len, bounds_.x, bounds_.y,
                        bounds_.w, bounds_.h, 0, 0, 1.0f, 1.0f,
                        datum_t::middle_center)
            : g.drawPngFile(card_->image, bounds_.x, bounds_.y, bounds_.w,
                            bounds_.h, 0, 0, 1.0f, 1.0f, datum_t::middle_center);
    if (!ok) {
        ESP_LOGW(kTag, "drawPngFile failed for %s", card_->image);
        // Fall back to a large letter so the card is still usable.
        g.fillRect(bounds_.x, bounds_.y, bounds_.w, bounds_.h, theme::kWhite);
        char letter[2] = {card_->letter, '\0'};
        const text::FontPick pick{&fonts::FreeSansBold24pt7b, 3};
        text::drawOutlined(g, letter, bounds_.centerX(), bounds_.centerY(), pick,
                           theme::kYellow, theme::kInk, 4);
        const text::FontPick small{&fonts::FreeSansBold9pt7b, 1};
        text::draw(g, "picture missing", bounds_.centerX(),
                   static_cast<int16_t>(bounds_.bottom() - 24), small, theme::kRed,
                   textdatum_t::middle_center);
    }
}

void CardView::drawWord(M5GFX& g) {
    ESP_LOGD(kTag, "drawWord: \"%s\"", card_->display);
    const Rect& wb = word_bounds_;

    // Leave room for the outline on both sides when fitting.
    const int16_t avail_w = static_cast<int16_t>(
        wb.w - 2 * (theme::kWordOutlineRadius + theme::kWordSideMargin));
    const int16_t avail_h =
        static_cast<int16_t>(wb.h - 2 * theme::kWordOutlineRadius);

    const text::FontPick pick = text::pickForBox(g, card_->display, avail_w, avail_h);

    const uint32_t accent =
        theme::kAccentGrapheme ? theme::kGraphemeFill : theme::kWordFill;

    ESP_LOGD(kTag, "drawWord: scale=%.2f h=%dpx w=%dpx in box %dx%d", pick.size,
             static_cast<int>(g.fontHeight()), static_cast<int>(g.textWidth(card_->display)),
             static_cast<int>(avail_w), static_cast<int>(avail_h));
    text::drawWordWithAccent(g, card_->display, card_->span_start, card_->span_len,
                             wb.centerX(), wb.centerY(), pick, theme::kWordFill,
                             accent, theme::kWordOutline,
                             theme::kWordOutlineRadius);
}

void CardView::drawPlaceholder(M5GFX& g) {
    g.fillRect(bounds_.x, bounds_.y, bounds_.w, bounds_.h, theme::kWhite);

    const text::FontPick big{&fonts::FreeSansBold24pt7b, 2};
    text::drawOutlined(g, "?", bounds_.centerX(),
                       static_cast<int16_t>(bounds_.centerY() - 40), big,
                       theme::kYellow, theme::kInk, 4);

    const text::FontPick mid{&fonts::FreeSansBold12pt7b, 1};
    const text::FontPick small{&fonts::FreeSansBold9pt7b, 1};
    if (ph1_[0]) {
        text::draw(g, ph1_, bounds_.centerX(),
                   static_cast<int16_t>(bounds_.centerY() + 40), mid, theme::kRed,
                   textdatum_t::middle_center);
    }
    if (ph2_[0]) {
        text::draw(g, ph2_, bounds_.centerX(),
                   static_cast<int16_t>(bounds_.centerY() + 70), small, theme::kInk,
                   textdatum_t::middle_center);
    }
}

}  // namespace ui
