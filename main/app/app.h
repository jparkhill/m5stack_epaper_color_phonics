/*
 * Application state machine.
 *
 * Interaction model (one panel refresh per action, because a Spectra 6
 * refresh costs ~10 seconds):
 *
 *   TOP button    -> next card: picture + word appear, then the narration plays
 *   MIDDLE button -> jump to the next letter of the alphabet
 *   BOTTOM button -> replay the current narration (NO panel refresh, instant)
 *
 * The LED gives immediate feedback because the picture is ~10s away: blue
 * while the panel refreshes, green while speaking.
 *
 * Console commands are delivered through a FreeRTOS queue rather than called
 * directly, so the REPL task never touches the panel or the SD card while the
 * app task is mid-refresh.
 */
#pragma once

#include <cstdint>
#include <esp_err.h>

namespace app {

/// Seconds between idle repaints purely to advance the clock.
///
/// Deliberately long: every repaint is a full ~10s panel refresh, and e-paper
/// panels have a finite number of cycles. At 300s the displayed time can be up
/// to five minutes stale while idle; any button press repaints immediately.
constexpr uint32_t kIdleClockRefreshSec = 300;

/// Seconds between SHT40 samples. Cheap (I2C only, no panel refresh) -- the
/// fresh value is shown at the next repaint.
constexpr uint32_t kSensorSampleSec = 30;

esp_err_t init();

/// Main loop. Does not return.
[[noreturn]] void run();

// --- Requests from the console task (thread-safe, queued) ------------------
void requestNextCard();
void requestNextLetter();
void requestReplay();
void requestRepaint();
void requestCard(char letter, int nth);
void requestStatusDump();

}  // namespace app
