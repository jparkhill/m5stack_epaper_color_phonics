/*
 * The three front buttons.
 *
 * M5Unified reads them as active-low GPIOs and exposes them as BtnA/BtnB/BtnC
 * in GPIO order 9, 10, 1 (see btn_rawstate_bits for board_M5PaperColor).
 *
 * Which one is physically the TOP button is not documented in any M5 source,
 * so the logical->physical mapping is a single table below. Run the `btn`
 * console command, press each button, and if the labels are wrong swap the
 * three entries -- nothing else in the codebase needs to change.
 */
#pragma once

#include <cstdint>

namespace hal::input {

/// Logical roles rather than physical positions.
///
/// The device has four buttons: one on top and three on the side. The lowest
/// side button is a hardware power button wired to the PMIC and is not
/// readable as a GPIO, so exactly three are visible to firmware -- the top one
/// and the two upper side ones.
///
/// Which GPIO is which physical button is not documented anywhere in the M5
/// sources, so BOTH kCycleA and kCycleB advance to the next card. That way
/// the two "cycle" buttons behave as asked no matter how they map, and the
/// third gets replay. Use the `btn` console command to identify them.
enum class Button : uint8_t { kCycleA = 0, kCycleB = 1, kExtra = 2, kCount = 3 };

void init();

/// Pump M5Unified's button state machine. Call once per app loop.
void update();

/// True exactly once per physical press (rising edge of "was clicked").
bool wasPressed(Button b);

/// True while the button is down.
bool isHeld(Button b);

/// True once when the button has been held for at least `ms`.
bool wasHeldFor(Button b, uint32_t ms);

/// "A=up B=DOWN C=up" style snapshot for the `btn` console command.
const char* rawSnapshot();

/// Human label, e.g. "top (BtnA/GPIO9)".
const char* label(Button b);

}  // namespace hal::input
