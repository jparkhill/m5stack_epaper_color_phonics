/*
 * Interactive serial console (USB-Serial-JTAG REPL).
 *
 * Exists so a debug cycle does not require a reflash. Commands that touch the
 * panel or the SD card are queued to the app task rather than executed inline.
 */
#pragma once

#include <esp_err.h>

namespace app::console {

/// Register commands and start the REPL.
///
/// MUST NOT be called unless a USB host is attached. Starting it installs the
/// USB-Serial-JTAG driver and switches the console VFS onto it, and with no
/// host present that blocks the calling task indefinitely -- the boot simply
/// stops. Measured: a board left unplugged sat between DECK-LOADED and the
/// first panel refresh for 78 seconds, and on another occasion 10.3 hours,
/// resuming the instant a cable was connected. The only outward sign is the
/// LED animation (started earlier, on CPU1) with a frozen screen and no boot
/// chime.
///
/// Idempotent: repeated calls after a successful start are no-ops, so the app
/// loop can keep trying until a host appears.
esp_err_t start();

/// True once the REPL is running.
bool started();

}  // namespace app::console
