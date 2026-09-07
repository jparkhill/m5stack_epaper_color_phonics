#include "hal/hal_display.h"
#include "ui/theme.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_psram.h>
#include <M5Unified.hpp>

namespace hal::display {
namespace {

constexpr const char* kTag = "display";

uint32_t s_last_refresh_ms = 0;
uint32_t s_refresh_count = 0;

epd_mode_t toEpdMode(RefreshMode mode) {
    switch (mode) {
        case RefreshMode::kImage:    return epd_mode_t::epd_fastest;
        case RefreshMode::kMixed:    return epd_mode_t::epd_quality;
        case RefreshMode::kTextOnly: return epd_mode_t::epd_text;
    }
    return epd_mode_t::epd_fastest;
}

}  // namespace

esp_err_t init() {
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(kTag, "PSRAM missing -- Panel_ED2208 cannot allocate its framebuffer");
        return ESP_ERR_NO_MEM;
    }

    auto cfg = M5.config();
    cfg.clear_display = false;   // we paint a real first frame ourselves
    // hal_audio owns the ES8311 codec and the I2S channel directly. Letting
    // M5Unified manage the speaker spawns its spk_task, whose fixed ~1280
    // bytes of stack headroom (see hal_audio.h) overflows on this board.
    cfg.internal_spk  = false;
    cfg.internal_mic  = false;   // not used; leaves I2S_NUM_1 free
    cfg.internal_imu  = false;   // no IMU on this board
    cfg.internal_rtc  = false;   // M5Unified's RTC class does not know RX8130;
                                 // hal_rtc drives it directly instead.
    cfg.output_power  = true;
    cfg.led_brightness = 0;      // hal_power owns the RGB LEDs
    M5.begin(cfg);

    const auto board = M5.getBoard();
    if (board != m5::board_t::board_M5PaperColor) {
        ESP_LOGE(kTag, "board autodetect returned %d, expected board_M5PaperColor (%d)",
                 static_cast<int>(board),
                 static_cast<int>(m5::board_t::board_M5PaperColor));
        ESP_LOGE(kTag, "check the internal I2C bus: autodetect keys off RTC 0x32 + PMIC 0x6E");
        return ESP_ERR_NOT_FOUND;
    }

    auto& g = M5.Display;

    // ----------------------------------------------------------------------
    // CRITICAL: turn off LovyanGFX's auto-display.
    //
    // Panel_FrameBufferBase::init() sets _auto_display = true whenever
    // LGFX_USE_CACHE_WRITEBACK_ADDR is defined -- which it always is on an
    // ESP32-S3 with a PSRAM framebuffer. That makes endWrite() call display()
    // after EVERY drawing operation, so a single fillRect or drawString
    // triggers a full physical Spectra 6 refresh. Measured: a constant
    // 16.73 s per drawing call, whether it touched 1,200 pixels or 240,000.
    // The screen appears to refresh in an endless cycle and a frame never
    // finishes being composed.
    //
    // With it off we compose the entire frame in the PSRAM framebuffer and
    // call display() exactly once. Safe for this panel: Panel_ED2208's
    // _exec_transfer() reads the framebuffer with the CPU rather than DMA, so
    // the cache write-back that _auto_display exists to automate is not
    // needed here.
    // ----------------------------------------------------------------------
    g.setAutoDisplay(false);
    ESP_LOGI(kTag, "auto-display OFF: one panel refresh per composed frame");

    g.setRotation(0);  // portrait 400x600
    if (g.width() != ui::theme::kScreenW || g.height() != ui::theme::kScreenH) {
        ESP_LOGW(kTag, "panel reports %dx%d but theme assumes %dx%d",
                 g.width(), g.height(), ui::theme::kScreenW, ui::theme::kScreenH);
    }
    ESP_LOGI(kTag, "panel %dx%d, 6-colour Spectra 6, framebuffer in PSRAM",
             g.width(), g.height());
    return ESP_OK;
}

M5GFX& gfx() { return M5.Display; }

void beginFrame() {
    // startWrite() holds the panel transaction open for the whole frame so
    // the per-operation bookkeeping happens once, not per primitive.
    M5.Display.startWrite();
    M5.Display.fillScreen(ui::theme::kBackground);
}

uint32_t present(RefreshMode mode) {
    auto& g = M5.Display;
    const int64_t t0 = esp_timer_get_time();

    g.endWrite();   // closes the transaction opened by beginFrame()
    g.setEpdMode(toEpdMode(mode));
    ESP_LOGI(kTag, "composing done; pushing one panel refresh (blocking ~17s)");
    g.display();

    s_last_refresh_ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
    ++s_refresh_count;
    ESP_LOGI(kTag, "panel refresh #%lu took %lu ms (mode=%d)",
             (unsigned long)s_refresh_count, (unsigned long)s_last_refresh_ms,
             static_cast<int>(mode));
    return s_last_refresh_ms;
}

uint32_t lastRefreshMs() { return s_last_refresh_ms; }
uint32_t refreshCount() { return s_refresh_count; }

}  // namespace hal::display
