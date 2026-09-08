/*
 * Staged boot tracing.
 *
 * Purpose: make a cold start legible over the USB serial console so a failure
 * can be diagnosed from one paste of the log, without a debugger and without
 * reflashing to add printfs.
 *
 * Usage:
 *     boot::begin();                       // banner + chip/reset/app dump
 *     boot::stage("microSD");
 *     if (ok) boot::ok("32.0GB FAT32");
 *     else    boot::fail(err, "mount failed");
 *     ...
 *     boot::summary();                     // timing table + verdict
 *
 * Every stage is timed, and a failed stage does not abort the boot: the app
 * degrades (e.g. no SD -> show a diagnostic screen) so the device still comes
 * up far enough to talk to you.
 */
#pragma once

#include <cstdint>
#include <esp_err.h>

namespace boot {

/// Print the banner and dump chip, reset-reason, memory and app metadata.
void begin();

/// Open a new stage. Closes any still-open stage as OK.
void stage(const char* name);

/// Close the current stage successfully. `detail` is optional printf-style.
void ok(const char* fmt = nullptr, ...) __attribute__((format(printf, 1, 2)));

/// Close the current stage as failed and record the error.
void fail(esp_err_t err, const char* fmt = nullptr, ...) __attribute__((format(printf, 2, 3)));

/// Close the current stage as intentionally skipped.
void skip(const char* fmt = nullptr, ...) __attribute__((format(printf, 1, 2)));

/// Emit the stage timing table plus a PASS/DEGRADED verdict.
void summary();

/// True if any stage failed.
bool anyFailed();

/// Human-readable summary of the first failure, or nullptr.
const char* firstFailure();

/// Probe every address on the internal I2C bus and report known devices.
void i2cScan();

/// Log internal + PSRAM heap free/largest-block under a label.
void heapReport(const char* label);

/// Human-readable reset reason for the current boot. Available after
/// begin(), and repeated in `stat` because the boot banner scrolls away.
const char* resetReasonText();

/// Milliseconds since begin().
uint32_t elapsedMs();

}  // namespace boot
