/*
 * M5PM1 PMIC access: microSD card-detect and the 2-LED RGB status chain.
 *
 * M5GFX's board autodetect already raises the e-paper rail (PMIC GPIO0) and
 * the microSD rail (PMIC GPIO3) before the panel comes up, so this module
 * only handles what nobody else does: arming card detect (GPIO4) and reading
 * it (GPIO1), plus the status LEDs used as boot-progress feedback.
 */
#pragma once

#include <cstdint>
#include <esp_err.h>

namespace hal::power {

/// Attach to the PMIC over the internal I2C bus. Call after display init
/// (M5.begin brings the bus up).
esp_err_t init();

/// True when the PMIC responded to init().
bool available();

/// Read the card-detect line. Returns false if the PMIC is unavailable.
bool sdCardInserted();

/// Cut every rail via the PMIC. Does not return on success -- the board is
/// off and only the hardware power button brings it back.
///
/// The e-paper panel is bistable, so whatever is on screen when this is
/// called stays on screen indefinitely at zero power.
void powerOff();

// --- Status LEDs -----------------------------------------------------------
// Used as immediate feedback: an e-paper refresh takes ~10s, so the LED is
// the only way to tell a child (or you) that a button press registered.

enum class Led : uint8_t {
    kIdle,      // off
    kBusy,      // working (blue)
    kSpeaking,  // audio playing (green)
    kError,     // something failed (red)
};

void ledSet(Led state);
void ledBrightness(uint8_t brightness);

/// Continuously cycle the two shoulder LEDs through a rainbow, to hold a
/// child's attention while the (slow) e-paper refresh happens.
///
/// Runs in its own task pinned to CPU1 so it keeps animating straight through
/// the ~10s blocking panel refresh on CPU0.
void ledRainbowStart();
void ledRainbowStop();
bool ledRainbowRunning();

}  // namespace hal::power
