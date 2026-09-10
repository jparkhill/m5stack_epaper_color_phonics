/*
 * Power-event ring buffer in RTC slow memory.
 *
 * Every sleep/wake/boot transition is recorded so a failure can be read back
 * AFTER the fact, instead of needing a serial capture at exactly the right
 * moment. That mattered here: the device kept coming back "LEDs only", and
 * every diagnosis attempt was guesswork because the interesting event had
 * already scrolled past -- or happened with no cable attached at all.
 *
 * Storage is RTC_NOINIT_ATTR, i.e. RTC slow memory. It survives BOTH deep
 * sleep and a software/watchdog reset, so a wake is recorded in the same log
 * as the sleep that preceded it. It is lost only on a true power removal,
 * which is itself useful information: an empty log means the rails actually
 * dropped.
 *
 * 128 events x 16 bytes = 2 KB of the 8 KB RTC slow region. Oldest events are
 * overwritten, so it can never grow.
 */
#pragma once

#include <cstdint>

namespace boot::plog {

enum class Event : uint8_t {
    kBoot = 0,        // app_main reached; detail = reset reason
    kSleepEnter,      // about to call esp_deep_sleep_start()
    kWake,            // boot whose reset reason was DEEPSLEEP; detail = cause
    kRailsDown,       // PMIC reports 3V3/5V off while we are still running
    kPowerOffCmd,     // PMIC shutdown commanded
    kReady,           // first card drawn, app loop entered
    kSdMounted,
    kSdFailed,
    kDeckLoaded,
    kPresentStart,    // about to block on a panel refresh
    kPresentDone,     // refresh returned; detail = milliseconds it took
    kChimeDone,       // boot chime finished
    kBattery,         // detail = cell millivolts
};

/// Append an event. Safe to call before init(); the first call initialises the
/// ring if the magic is absent (i.e. after a real power loss).
void record(Event ev, uint32_t detail = 0);

/// Prepare the ring and record the boot event, including the reset reason and
/// -- if this was a deep-sleep wake -- the wake cause and the GPIO that did
/// it. Call early in app_main.
void begin();

/// Print the whole ring, oldest first.
void dump();

/// Wipe it.
void clear();

/// Events recorded since the ring was last (re)initialised.
uint32_t count();

/// True if the ring survived from a previous boot, i.e. no power loss.
bool carriedOver();

}  // namespace boot::plog
