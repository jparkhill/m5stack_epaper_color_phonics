/*
 * Minimal widget contract.
 *
 * The screen is partitioned into fixed rectangular regions (see theme.h) and
 * each region is owned by exactly one Widget. Adding a new panel of
 * information means writing a Widget and handing it a Rect -- no other file
 * needs to change.
 *
 * A note on redraw: Panel_ED2208 has no true partial refresh. Every
 * display() call re-transmits the whole framebuffer and runs a full
 * POWER_ON/REFRESH/POWER_OFF cycle (~10s). So widgets always draw in full and
 * the Screen batches them into a single panel refresh.
 */
#pragma once

#include <cstdint>
#include <M5GFX.h>

namespace ui {

struct Rect {
    int16_t x{};
    int16_t y{};
    int16_t w{};
    int16_t h{};

    constexpr int16_t right() const { return static_cast<int16_t>(x + w); }
    constexpr int16_t bottom() const { return static_cast<int16_t>(y + h); }
    constexpr int16_t centerX() const { return static_cast<int16_t>(x + w / 2); }
    constexpr int16_t centerY() const { return static_cast<int16_t>(y + h / 2); }
};

class Widget {
public:
    virtual ~Widget() = default;

    void setBounds(const Rect& r) { bounds_ = r; }
    const Rect& bounds() const { return bounds_; }

    /// Short identifier used in boot traces and the `ui` console command.
    virtual const char* name() const = 0;

    /// Render into the panel framebuffer. Must stay inside bounds().
    virtual void draw(M5GFX& g) = 0;

protected:
    Rect bounds_{};
};

}  // namespace ui
