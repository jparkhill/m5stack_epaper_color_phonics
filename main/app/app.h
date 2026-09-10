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

/// Seconds of no user activity before the device sleeps -- ONLY when the
/// timeout has been explicitly enabled with `autosleep`.
///
/// DISABLED BY DEFAULT, and that is a deliberate retreat. Two mechanisms were
/// tried and both can strand the device:
///
///   * PMIC SYS_CMD_SHUTDOWN cuts the peripheral rails without resetting the
///     chip on battery power (M5GFX's own source says so), leaving the app
///     running with a dead panel, SD card and codec.
///   * ESP32-S3 deep sleep resets cleanly in principle, but the RTC domain is
///     fed from the same 3.3V rail. A power event takes the RTC domain with
///     it, destroying both the sleep state and the EXT1 wake configuration --
///     confirmed by the power log coming back reinitialised with
///     reset=POWERON and no SLEEP-ENTER event.
///
/// The e-paper panel is bistable, so PWR_KEY already gives the desired end
/// state: the last card stays on screen at zero draw. An automatic timeout
/// adds a way to strand the device without adding much.
constexpr uint32_t kIdleSleepSec = 15 * 60;

/// Whether to auto power-off while a USB host is attached.
///
/// Defaults to false. With it true, the device would cut power in the middle
/// of a `idf.py monitor` session or a flash -- the port simply vanishes and
/// the board looks bricked until someone presses PWR_KEY. A device on a USB
/// cable is also, in practice, one that is being worked on or charged. On
/// battery the timeout applies normally, which is the case that matters.
constexpr bool kSleepWhileUsbConnected = false;

/// Refuse to auto power-off below this battery percentage.
///
/// Counter-intuitive but deliberate. A low cell is exactly when the PMIC will
/// not restart the rails from battery (see BATT_LVP, register 0x08), so
/// sleeping then produces a board that ignores PWR_KEY entirely and only
/// revives when USB is plugged in -- which reads as "bricked". A device that
/// stays on until it visibly dies is far less confusing than one that sleeps
/// and cannot be woken.
constexpr uint8_t kMinBatteryPercentToSleep = 20;

/// Seconds of no further letter-steps before the panel is repainted to move
/// the highlight.
///
/// Stepping the taught letter used to force a full ~16s refresh per press,
/// which made walking through "apple" an 80-second exercise -- and the only
/// visual change is which letter is coloured. So the audio now plays
/// immediately and the repaint is deferred until the child stops pressing.
/// Sound is instant; the highlight catches up shortly after.
constexpr uint32_t kLetterStepRepaintSec = 4;

esp_err_t init();

/// Main loop. Does not return.
[[noreturn]] void run();

// --- Requests from the console task (thread-safe, queued) ------------------
void requestNextCard();
void requestNextLetter();

/// Step the taught letter to the next character of the CURRENT word:
/// Apple -> aPple -> apPle -> appLe -> applE, wrapping at the end.
void requestNextLetterInWord();
void requestReplay();
void requestRepaint();
void requestCard(char letter, int nth);
void requestStatusDump();

/// Power the device off now (the `sleep` console command). Same path the idle
/// timeout takes, so it is the way to test sleep without waiting 15 minutes.
void requestSleep();

/// Enable/disable the idle auto power-off at runtime, persisted in NVS.
///
/// Exists because a powered-off board is easy to mistake for a broken one.
/// PWR_KEY is: quick press = on, double press = off, HOLD = download mode --
/// and holding it (the intuitive "power on" gesture) parks the chip in
/// download mode, where USB enumerates and esptool works but the app never
/// runs and nothing is printed at all. Being able to switch the timeout off
/// from the console beats reflashing to find out.
void setIdleSleepEnabled(bool enabled);
bool idleSleepEnabled();

/// Seconds remaining before the idle timeout fires.
uint32_t secondsUntilSleep();

}  // namespace app
