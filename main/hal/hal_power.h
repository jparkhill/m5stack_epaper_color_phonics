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

/// Battery state, read from the PMIC's own voltage sensor.
struct Battery {
    uint16_t millivolts;   // raw reading
    uint8_t percent;       // 0-100, from a Li-ion discharge curve
    bool charging;         // running from 5V rather than the cell
    bool valid;
};

/// Read the battery voltage (PMIC registers 0x22/0x23, in mV) and the current
/// power source (0x04). Cheap: three I2C byte reads.
Battery readBattery();

/// Dump battery voltage, power source, the low-voltage-protection threshold
/// and the PMIC's I2C-idle-sleep setting.
///
/// Exists to settle why the board sometimes refuses to wake on a PWR_KEY
/// press but comes straight up when USB is plugged in. If the cell is below
/// BATT_LVP the PMIC will not start the rails from the battery, while a valid
/// 5V input satisfies it -- which produces exactly that behaviour.
void dumpPowerState();

/// Hex-dump PMIC registers [first, last]. For working out an encoding from
/// real values instead of guessing at a datasheet comment.
void dumpRegisters(uint8_t first, uint8_t last);

/// Enter ESP32-S3 DEEP SLEEP, waking on any of the three front buttons.
/// Never returns: waking is a full chip reset.
///
/// This -- not the PMIC -- is how the idle timeout sleeps, and the reason is
/// specific. M5GFX's own source notes that "PMIC is always-on powered, and
/// with battery power, shutdown doesn't reset the chip". So the PMIC's
/// SYS_CMD_SHUTDOWN cuts the peripheral rails while the ESP32-S3 keeps
/// executing: the LED task carries on (GPIO21 is driven from the chip's own
/// supply) while the panel, SD card and codec are dead. The device looks
/// half-alive -- shoulder LEDs cycling, nothing else responding -- and no
/// number of PWR_KEY presses fixes it, because the chip never resets.
///
/// Deep sleep is a proper state machine by comparison: the CPU and
/// peripherals power down, the RTC domain keeps the wake logic alive, and a
/// wake event resets the chip so it boots from scratch
/// (esp_reset_reason() == ESP_RST_DEEPSLEEP).
///
/// Wake is EXT1 on the three button GPIOs (9, 10, 1), all of which are within
/// the ESP32-S3's 22 RTC-capable pins. PWR_KEY cannot be a wake source: it is
/// a PMIC pin, not an ESP32 GPIO, so the RTC domain cannot see it.
[[noreturn]] void enterDeepSleep();

/// Cut every rail via the PMIC -- a true off, reached only from the explicit
/// `poweroff` command.
///
/// On battery this may NOT reset the chip (see enterDeepSleep). If the rails
/// are still up shortly afterwards, this falls back to deep sleep rather than
/// leaving the device half-powered.
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
