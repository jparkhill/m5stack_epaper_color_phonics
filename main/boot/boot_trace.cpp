#include "boot/boot_trace.h"
#include "hal/hal_pins.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_psram.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <M5Unified.hpp>

namespace boot {
namespace {

constexpr const char* kTag = "boot";
constexpr int kMaxStages = 32;
constexpr int kDetailLen = 80;

enum class Status : uint8_t { kRunning, kOk, kFailed, kSkipped };

struct StageRec {
    const char* name;
    uint32_t start_us;
    uint32_t dur_us;
    Status status;
    esp_err_t err;
    char detail[kDetailLen];
};

StageRec s_stages[kMaxStages];
int s_count = 0;
int s_open = -1;              // index of the currently open stage
int64_t s_boot_start_us = 0;
bool s_any_failed = false;
char s_first_failure[120] = {0};

const char* resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON (cold start / battery insert)";
        case ESP_RST_EXT:       return "EXT (external reset pin)";
        case ESP_RST_SW:        return "SW (esp_restart)";
        case ESP_RST_PANIC:     return "PANIC (crash -- check the backtrace above!)";
        case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog)";
        case ESP_RST_TASK_WDT:  return "TASK_WDT (task watchdog)";
        case ESP_RST_WDT:       return "WDT (other watchdog)";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP (woke from deep sleep)";
        case ESP_RST_BROWNOUT:  return "BROWNOUT (supply dipped -- check USB/battery)";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_USB:       return "USB (host-triggered reset)";
        case ESP_RST_JTAG:      return "JTAG";
        default:                return "UNKNOWN";
    }
}

void closeOpenStage(Status st, esp_err_t err, const char* detail) {
    if (s_open < 0) return;
    StageRec& rec = s_stages[s_open];
    rec.dur_us = static_cast<uint32_t>(esp_timer_get_time()) - rec.start_us;
    rec.status = st;
    rec.err = err;
    if (detail && *detail) {
        std::snprintf(rec.detail, sizeof(rec.detail), "%s", detail);
    }

    const float ms = rec.dur_us / 1000.0f;
    switch (st) {
        case Status::kOk:
            if (rec.detail[0]) {
                ESP_LOGI(kTag, "  [ OK ] %-18s %7.1f ms  %s", rec.name, ms, rec.detail);
            } else {
                ESP_LOGI(kTag, "  [ OK ] %-18s %7.1f ms", rec.name, ms);
            }
            break;
        case Status::kFailed:
            ESP_LOGE(kTag, "  [FAIL] %-18s %7.1f ms  %s (%s)", rec.name, ms,
                     rec.detail[0] ? rec.detail : "no detail", esp_err_to_name(err));
            if (!s_any_failed) {
                std::snprintf(s_first_failure, sizeof(s_first_failure), "%s: %s (%s)",
                              rec.name, rec.detail[0] ? rec.detail : "failed",
                              esp_err_to_name(err));
            }
            s_any_failed = true;
            break;
        case Status::kSkipped:
            ESP_LOGW(kTag, "  [SKIP] %-18s %7.1f ms  %s", rec.name, ms,
                     rec.detail[0] ? rec.detail : "");
            break;
        default:
            break;
    }
    s_open = -1;
}

}  // namespace

void begin() {
    s_boot_start_us = esp_timer_get_time();
    s_count = 0;
    s_open = -1;
    s_any_failed = false;
    s_first_failure[0] = '\0';

    // Our own modules get DEBUG; IDF internals stay at their default so the
    // log remains readable.
    esp_log_level_set("boot", ESP_LOG_DEBUG);
    esp_log_level_set("app", ESP_LOG_DEBUG);
    esp_log_level_set("deck", ESP_LOG_DEBUG);
    esp_log_level_set("sd", ESP_LOG_DEBUG);
    esp_log_level_set("audio", ESP_LOG_DEBUG);
    esp_log_level_set("rtc", ESP_LOG_DEBUG);
    esp_log_level_set("sensors", ESP_LOG_DEBUG);
    esp_log_level_set("display", ESP_LOG_DEBUG);
    esp_log_level_set("input", ESP_LOG_DEBUG);
    esp_log_level_set("power", ESP_LOG_DEBUG);
    esp_log_level_set("screen", ESP_LOG_DEBUG);
    esp_log_level_set("cardview", ESP_LOG_DEBUG);

    const esp_app_desc_t* app = esp_app_get_description();

    std::printf("\n\n");
    std::printf("+============================================================+\n");
    std::printf("|  PHONICS CARDS  --  M5Stack PaperColor (C151)              |\n");
    std::printf("+============================================================+\n");

    ESP_LOGI(kTag, "app          : %s v%s", app->project_name, app->version);
    ESP_LOGI(kTag, "built        : %s %s (IDF %s)", app->date, app->time, app->idf_ver);
    ESP_LOGI(kTag, "reset reason : %s", resetReasonName(esp_reset_reason()));

    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    ESP_LOGI(kTag, "chip         : ESP32-S3 rev v%d.%d, %d core%s",
             chip.revision / 100, chip.revision % 100, chip.cores,
             chip.cores == 1 ? "" : "s");

    uint32_t flash_size = 0;
    if (esp_flash_get_size(nullptr, &flash_size) == ESP_OK) {
        ESP_LOGI(kTag, "flash        : %lu MB", (unsigned long)(flash_size / (1024 * 1024)));
    } else {
        ESP_LOGW(kTag, "flash        : size query failed");
    }

    // PSRAM is not optional on this board: Panel_ED2208 puts its 400*600*3
    // RGB888 framebuffer (~703 KB) in PSRAM and init() fails without it.
    if (esp_psram_is_initialized()) {
        const size_t psram = esp_psram_get_size();
        ESP_LOGI(kTag, "PSRAM        : %u KB (octal) -- required by the panel driver",
                 (unsigned)(psram / 1024));
    } else {
        ESP_LOGE(kTag, "PSRAM        : NOT INITIALISED. Panel_ED2208 needs a ~703 KB");
        ESP_LOGE(kTag, "               framebuffer in PSRAM and WILL fail to init.");
        ESP_LOGE(kTag, "               Check CONFIG_SPIRAM_MODE_OCT / CLK_IO=30 / CS_IO=26.");
    }

    heapReport("at boot");
    std::printf("+------------------------------------------------------------+\n");
    ESP_LOGI(kTag, "starting staged init");
}

void stage(const char* name) {
    closeOpenStage(Status::kOk, ESP_OK, nullptr);
    if (s_count >= kMaxStages) {
        ESP_LOGW(kTag, "stage table full, not tracing '%s'", name);
        return;
    }
    StageRec& rec = s_stages[s_count];
    rec.name = name;
    rec.start_us = static_cast<uint32_t>(esp_timer_get_time());
    rec.dur_us = 0;
    rec.status = Status::kRunning;
    rec.err = ESP_OK;
    rec.detail[0] = '\0';
    s_open = s_count;
    ++s_count;
    ESP_LOGD(kTag, "  ...... %s", name);
}

void ok(const char* fmt, ...) {
    char buf[kDetailLen] = {0};
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
    }
    closeOpenStage(Status::kOk, ESP_OK, buf);
}

void fail(esp_err_t err, const char* fmt, ...) {
    char buf[kDetailLen] = {0};
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
    }
    closeOpenStage(Status::kFailed, err, buf);
}

void skip(const char* fmt, ...) {
    char buf[kDetailLen] = {0};
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
    }
    closeOpenStage(Status::kSkipped, ESP_OK, buf);
}

void heapReport(const char* label) {
    const size_t i_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t i_big  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t p_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t p_big  = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ESP_LOGI(kTag, "heap %-10s: internal %u KB free (max block %u KB) | "
                   "PSRAM %u KB free (max block %u KB)",
             label, (unsigned)(i_free / 1024), (unsigned)(i_big / 1024),
             (unsigned)(p_free / 1024), (unsigned)(p_big / 1024));
}

void i2cScan() {
    ESP_LOGI(kTag, "I2C scan on SDA=%d SCL=%d:", hal::pins::kI2cSda, hal::pins::kI2cScl);
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
        if (!M5.In_I2C.scanID(addr, 100000)) continue;
        ++found;
        const char* who = "unknown";
        if (addr == hal::pins::kAddrRx8130) who = "RX8130 RTC";
        else if (addr == hal::pins::kAddrSht40) who = "SHT40 temp/humidity";
        else if (addr == hal::pins::kAddrPm1) who = "M5PM1 PMIC";
        ESP_LOGI(kTag, "    0x%02X  %s", addr, who);
    }
    if (found == 0) {
        ESP_LOGE(kTag, "    no devices found -- the internal I2C bus is dead.");
        ESP_LOGE(kTag, "    Expect RTC 0x%02X, SHT40 0x%02X, PMIC 0x%02X.",
                 hal::pins::kAddrRx8130, hal::pins::kAddrSht40, hal::pins::kAddrPm1);
    }
}

void summary() {
    closeOpenStage(Status::kOk, ESP_OK, nullptr);

    std::printf("+------------------------------------------------------------+\n");
    std::printf("|  BOOT SUMMARY                                              |\n");
    std::printf("+------------------------------------------------------------+\n");
    uint32_t total_us = 0;
    for (int i = 0; i < s_count; ++i) {
        const StageRec& r = s_stages[i];
        total_us += r.dur_us;
        const char* mark = "  ok  ";
        if (r.status == Status::kFailed) mark = " FAIL ";
        else if (r.status == Status::kSkipped) mark = " skip ";
        std::printf("| %-6s %-18s %8.1f ms  %-20s|\n", mark, r.name,
                    r.dur_us / 1000.0f, r.detail);
    }
    std::printf("+------------------------------------------------------------+\n");
    std::printf("|  total traced %8.1f ms   wall %8.1f ms              |\n",
                total_us / 1000.0f, elapsedMs() / 1.0f);
    if (s_any_failed) {
        std::printf("|  VERDICT: DEGRADED -- see [FAIL] above                      |\n");
        std::printf("|  first failure: %-42s|\n", s_first_failure);
    } else {
        std::printf("|  VERDICT: PASS -- all stages healthy                       |\n");
    }
    std::printf("+============================================================+\n");
    std::printf("Type `help` for debug commands.\n\n");
}

bool anyFailed() { return s_any_failed; }

const char* firstFailure() { return s_any_failed ? s_first_failure : nullptr; }

uint32_t elapsedMs() {
    return static_cast<uint32_t>((esp_timer_get_time() - s_boot_start_us) / 1000);
}

}  // namespace boot
