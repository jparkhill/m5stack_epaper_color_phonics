/*
 * e-paper display access.
 *
 * The panel is a 400x600 Spectra 6 (Panel_ED2208). Two facts drive the whole
 * UI design:
 *
 *  1. There is NO partial refresh. Every present() re-transmits the entire
 *     framebuffer and runs POWER_ON -> DISPLAY_REFRESH -> POWER_OFF, which
 *     blocks for roughly 10 seconds. So: draw everything, present once.
 *
 *  2. Only six ink colours exist. The driver keeps an RGB888 framebuffer in
 *     PSRAM and quantises at transmit time. Card images are pre-dithered on
 *     the host to those exact six RGB values, so they are presented with
 *     RefreshMode::kImage (no on-device dithering) to avoid dithering twice.
 */
#pragma once

#include <cstdint>
#include <esp_err.h>
#include <M5GFX.h>

namespace hal::display {

enum class RefreshMode : uint8_t {
    kImage,    // epd_fastest: straight nearest-colour, for pre-dithered art
    kMixed,    // epd_quality: dithered, for photos or un-processed images
    kTextOnly, // epd_text: tuned for flat colour + glyph edges
};

esp_err_t init();

/// The live panel instance (M5.Display).
M5GFX& gfx();

/// Clear the framebuffer to the background colour. Does not touch the panel.
void beginFrame();

/// Push the framebuffer to the panel. BLOCKS for ~10s. Returns elapsed ms.
uint32_t present(RefreshMode mode = RefreshMode::kImage);

/// Duration of the most recent present(), in ms.
uint32_t lastRefreshMs();

/// Number of panel refreshes since boot (e-paper panels have finite cycles).
uint32_t refreshCount();

}  // namespace hal::display
