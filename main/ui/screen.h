/*
 * Screen composition.
 *
 * Owns the region -> widget assignment and batches a full repaint into ONE
 * panel refresh. Adding a new information panel is: write a Widget, give it a
 * Rect from theme.h, register it here.
 */
#pragma once

#include "ui/ui_types.h"
#include "ui/w_cardview.h"
#include "ui/w_statusbar.h"
#include "hal/hal_display.h"

namespace ui {

class Screen {
public:
    void init();

    StatusBar& statusBar() { return status_; }
    CardView& cardView() { return card_; }

    /// Paint every widget into the framebuffer. Does NOT touch the panel.
    void render();

    /// render() then push to the panel. BLOCKS ~10s. Returns refresh ms.
    uint32_t present(hal::display::RefreshMode mode = hal::display::RefreshMode::kImage);

    /// Log the region table (the `ui` console command).
    void describe() const;

private:
    void drawChrome(M5GFX& g);
    static void drawArrowUp(M5GFX& g, int16_t cx, int16_t cy, int16_t size,
                            uint32_t color);
    static void drawArrowLeft(M5GFX& g, int16_t cx, int16_t cy, int16_t size,
                              uint32_t color);

    StatusBar status_;
    CardView card_;
    Widget* widgets_[2]{};
    size_t widget_count_{0};
};

}  // namespace ui
