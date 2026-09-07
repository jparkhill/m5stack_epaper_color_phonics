/*
 * microSD card (SPI mode) mounted at /sd.
 *
 * The card shares SPI2 with the e-paper panel, and M5GFX has already
 * initialised that bus by the time we get here -- so spi_bus_initialize()
 * returning ESP_ERR_INVALID_STATE is the expected, healthy path, not an error.
 *
 * Never read the card while a panel refresh is in flight. The app is
 * single-threaded through the card/panel path specifically to guarantee that.
 */
#pragma once

#include <cstdint>
#include <esp_err.h>
#include <sdmmc_cmd.h>

namespace hal::sd {

constexpr const char* kMountPoint = "/sd";

esp_err_t init();
bool mounted();

/// Negotiated bus clock, in kHz (the driver steps down on flaky cards).
uint32_t clockKhz();

/// Card capacity and free space in MB. Returns false if unmounted.
bool usage(uint64_t* total_mb, uint64_t* free_mb);

/// Log card identity, capacity, FAT usage. Safe when unmounted.
void dumpInfo();

/// Unmount and remount -- exposed as the `sd` console command so a card can
/// be swapped without power-cycling.
esp_err_t remount();

}  // namespace hal::sd
