/*
 * Card view: the square picture area plus the big outlined word beneath it.
 *
 * Two regions, one widget, because the picture and the word are a single unit
 * of meaning and always change together. Each gets its own Rect so the
 * geometry still lives entirely in theme.h.
 */
#pragma once

#include "ui/ui_types.h"
#include "content/deck.h"

namespace ui {

class CardView : public Widget {
public:
    const char* name() const override { return "cardview"; }

    /// Where the big word goes (below the square image area).
    void setWordBounds(const Rect& r) { word_bounds_ = r; }

    /// The card to render; nullptr renders the "insert an SD card" placeholder.
    void setCard(const content::Card* card) { card_ = card; }
    const content::Card* card() const { return card_; }

    /// Message shown instead of a card when the deck could not load.
    void setPlaceholder(const char* line1, const char* line2);

    void draw(M5GFX& g) override;

private:
    void drawImage(M5GFX& g);
    void drawWord(M5GFX& g);
    void drawPlaceholder(M5GFX& g);

    const content::Card* card_{nullptr};
    Rect word_bounds_{};
    char ph1_[48]{};
    char ph2_[48]{};
};

}  // namespace ui
