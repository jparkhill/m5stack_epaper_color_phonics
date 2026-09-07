/*
 * Interactive serial console (USB-Serial-JTAG REPL).
 *
 * Exists so a debug cycle does not require a reflash. Commands that touch the
 * panel or the SD card are queued to the app task rather than executed inline.
 */
#pragma once

#include <esp_err.h>

namespace app::console {

/// Register commands and start the REPL. Call last, after the app is up.
esp_err_t start();

}  // namespace app::console
