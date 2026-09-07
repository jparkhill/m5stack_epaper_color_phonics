#include "hal/hal_sd.h"
#include "hal/hal_pins.h"
#include "hal/hal_power.h"

#include <cstdio>

#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>

namespace hal::sd {
namespace {

constexpr const char* kTag = "sd";
constexpr spi_host_device_t kHost = SPI2_HOST;

// The reference firmware steps down through exactly these rates; slow cards
// and long flex cables fail to initialise at 20MHz but come up fine at 4MHz.
constexpr int kFreqLadderKhz[] = {20000, 10000, 4000};

sdmmc_card_t* s_card = nullptr;
bool s_mounted = false;
uint32_t s_clock_khz = 0;

esp_err_t ensureSpiBus() {
    spi_bus_config_t bus_cfg{};
    bus_cfg.mosi_io_num = pins::kSpiMosi;
    bus_cfg.miso_io_num = pins::kSpiMiso;
    bus_cfg.sclk_io_num = pins::kSpiSclk;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 8192;

    const esp_err_t err = spi_bus_initialize(kHost, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err == ESP_ERR_INVALID_STATE) {
        // Already up -- M5GFX initialised SPI2 for the panel. Expected.
        ESP_LOGD(kTag, "SPI2 already initialised (shared with the e-paper panel)");
        return ESP_OK;
    }
    return err;
}

esp_err_t tryMount(int freq_khz) {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = kHost;
    host.max_freq_khz = freq_khz;

    sdspi_device_config_t slot{};
    slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = kHost;
    slot.gpio_cs = pins::kSdCs;

    esp_vfs_fat_mount_config_t mount{};
    // Never reformat automatically: the card holds hand-generated assets and
    // silently wiping it would be far worse than failing to mount.
    mount.format_if_mount_failed = false;
    mount.max_files = 6;
    mount.allocation_unit_size = 16 * 1024;

    return esp_vfs_fat_sdspi_mount(kMountPoint, &host, &slot, &mount, &s_card);
}

}  // namespace

esp_err_t init() {
    if (s_mounted) return ESP_OK;

    if (power::available() && !power::sdCardInserted()) {
        ESP_LOGE(kTag, "no card detected (PMIC SD_DEC high). Insert a FAT32 microSD.");
        return ESP_ERR_NOT_FOUND;
    }

    const esp_err_t bus_err = ensureSpiBus();
    if (bus_err != ESP_OK) {
        ESP_LOGE(kTag, "SPI2 init failed: %s", esp_err_to_name(bus_err));
        return bus_err;
    }

    esp_err_t last = ESP_FAIL;
    for (const int khz : kFreqLadderKhz) {
        last = tryMount(khz);
        if (last == ESP_OK) {
            s_mounted = true;
            s_clock_khz = static_cast<uint32_t>(khz);
            ESP_LOGI(kTag, "mounted %s at %d kHz", kMountPoint, khz);
            return ESP_OK;
        }
        ESP_LOGW(kTag, "mount at %d kHz failed (%s); stepping down", khz,
                 esp_err_to_name(last));
    }

    if (last == ESP_FAIL) {
        ESP_LOGE(kTag, "card did not mount. Is it FAT32 (not exFAT)? exFAT is");
        ESP_LOGE(kTag, "compiled out of this build (FF_FS_EXFAT=0).");
    }
    return last;
}

bool mounted() { return s_mounted; }
uint32_t clockKhz() { return s_clock_khz; }

bool usage(uint64_t* total_mb, uint64_t* free_mb) {
    if (!s_mounted) return false;
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(kMountPoint, &total, &freeb) != ESP_OK) return false;
    if (total_mb) *total_mb = total / (1024 * 1024);
    if (free_mb) *free_mb = freeb / (1024 * 1024);
    return true;
}

void dumpInfo() {
    if (!s_mounted || s_card == nullptr) {
        ESP_LOGW(kTag, "no card mounted");
        return;
    }
    ESP_LOGI(kTag, "card   : %s", s_card->cid.name);
    ESP_LOGI(kTag, "type   : %s", (s_card->ocr & (1 << 30)) ? "SDHC/SDXC" : "SDSC");
    ESP_LOGI(kTag, "size   : %llu MB",
             (static_cast<uint64_t>(s_card->csd.capacity) * s_card->csd.sector_size) /
                 (1024ULL * 1024ULL));
    ESP_LOGI(kTag, "clock  : %lu kHz (negotiated)", (unsigned long)s_clock_khz);
    ESP_LOGI(kTag, "sector : %d bytes", s_card->csd.sector_size);

    uint64_t total_mb = 0, free_mb = 0;
    if (usage(&total_mb, &free_mb)) {
        ESP_LOGI(kTag, "FAT    : %llu MB total, %llu MB free", total_mb, free_mb);
    }
}

esp_err_t remount() {
    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount(kMountPoint, s_card);
        s_mounted = false;
        s_card = nullptr;
        s_clock_khz = 0;
        ESP_LOGI(kTag, "unmounted");
    }
    return init();
}

}  // namespace hal::sd
