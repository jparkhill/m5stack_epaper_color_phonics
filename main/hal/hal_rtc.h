/*
 * RX8130 real-time clock (internal I2C, 0x32).
 *
 * M5Unified's RTC_Class only speaks BM8563/PCF8563, so this talks to the
 * RX8130 directly. Only the timekeeping registers (0x10..0x16, BCD) and the
 * flag register (0x1E) are touched -- the control registers are deliberately
 * left alone.
 *
 * TIMEZONE POLICY: the RTC stores LOCAL wall-clock time and the ESP32's TZ is
 * pinned to UTC0, so no conversion ever happens and there are no DST bugs.
 * Whatever you push in with `time` is exactly what the screen shows.
 * `tools/set_time.py` pushes the host's local time after flashing.
 */
#pragma once

#include <cstdint>
#include <ctime>
#include <esp_err.h>

namespace hal::rtc {

struct DateTime {
    uint16_t year{2026};   // full year
    uint8_t month{1};      // 1-12
    uint8_t day{1};        // 1-31
    uint8_t hour{0};       // 0-23
    uint8_t minute{0};
    uint8_t second{0};
    uint8_t weekday{0};    // 0=Sunday
};

/// Probe the RTC, detect a lost-power condition, and seed the system clock.
/// If the RTC never held valid time, it is seeded from the firmware build
/// timestamp so the UI has something plausible to show, and a loud warning is
/// logged telling you to run the `time` command.
esp_err_t init();

bool available();

/// True if the RTC lost power since it was last set (VLF flag) -- meaning the
/// displayed time is a build-time guess rather than real.
bool timeIsSuspect();

bool get(DateTime* out);
bool set(const DateTime& dt);

/// Parse "YYYY-MM-DD HH:MM:SS" and write it to the RTC + system clock.
bool setFromString(const char* iso);

/// Copy the RTC into the ESP32 system clock (so localtime()/strftime work).
bool syncSystemClock();

/// Format the firmware build timestamp as a DateTime (fallback seed).
DateTime buildTimestamp();

}  // namespace hal::rtc
