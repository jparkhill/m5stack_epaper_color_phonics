#include "app/console_cmds.h"
#include "app/app.h"
#include "boot/boot_trace.h"
#include "content/deck.h"
#include "hal/hal_audio.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "hal/hal_pins.h"
#include "hal/hal_power.h"
#include "hal/hal_rtc.h"
#include "hal/hal_sd.h"
#include "hal/hal_sensors.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <driver/gpio.h>
#include <esp_console.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace app::console {
namespace {

constexpr const char* kTag = "console";

int cmdStat(int, char**) {
    requestStatusDump();
    return 0;
}

int cmdNext(int, char**) {
    std::printf("queued: next card (panel refresh takes ~10s)\n");
    requestNextCard();
    return 0;
}

int cmdLetter(int, char**) {
    std::printf("queued: next letter\n");
    requestNextLetter();
    return 0;
}

int cmdAgain(int, char**) {
    std::printf("queued: replay narration\n");
    requestReplay();
    return 0;
}

int cmdRepaint(int, char**) {
    std::printf("queued: full repaint\n");
    requestRepaint();
    return 0;
}

int cmdCard(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: card <A-Z> [index]\n");
        return 1;
    }
    const char letter = argv[1][0];
    const int nth = (argc >= 3) ? std::atoi(argv[2]) : 0;
    const int have = content::cardsForLetter(letter);
    if (have == 0) {
        std::printf("letter '%c' has no cards\n", letter);
        return 1;
    }
    std::printf("queued: letter %c card %d of %d\n", letter, nth % have, have);
    requestCard(letter, nth);
    return 0;
}

int cmdTime(int argc, char** argv) {
    if (argc < 2) {
        hal::rtc::DateTime dt{};
        if (hal::rtc::get(&dt)) {
            std::printf("rtc: %04u-%02u-%02u %02u:%02u:%02u%s\n", dt.year, dt.month,
                        dt.day, dt.hour, dt.minute, dt.second,
                        hal::rtc::timeIsSuspect() ? "  (SUSPECT)" : "");
        } else {
            std::printf("rtc unavailable\n");
        }
        std::printf("usage: time YYYY-MM-DD HH:MM:SS\n");
        return 0;
    }
    // Re-join argv so both `time "2026-09-07 20:15:00"` and
    // `time 2026-09-07 20:15:00` work.
    char joined[64] = {0};
    size_t used = 0;
    for (int i = 1; i < argc && used < sizeof(joined) - 1; ++i) {
        const int n = std::snprintf(joined + used, sizeof(joined) - used, "%s%s",
                                    i > 1 ? " " : "", argv[i]);
        if (n <= 0) break;
        used += static_cast<size_t>(n);
    }
    if (!hal::rtc::setFromString(joined)) {
        std::printf("failed to set time from \"%s\"\n", joined);
        return 1;
    }
    std::printf("time set. Run `repaint` to show it.\n");
    return 0;
}

int cmdBtn(int, char**) {
    // Reads the GPIOs directly instead of going through M5.update(), so this
    // cannot race the app task's own button polling.
    std::printf("Press each button. Watching for 8s...\n");
    std::printf("(all three are active-low: DOWN means pressed)\n");
    int last_a = -1, last_b = -1, last_c = -1;
    for (int i = 0; i < 800; ++i) {
        const int a = gpio_get_level(hal::pins::kBtnA);
        const int b = gpio_get_level(hal::pins::kBtnB);
        const int c = gpio_get_level(hal::pins::kBtnC);
        if (a != last_a || b != last_b || c != last_c) {
            std::printf("  BtnA/GPIO%-2d %-4s | BtnB/GPIO%-2d %-4s | BtnC/GPIO%-2d %-4s\n",
                        hal::pins::kBtnA, a ? "up" : "DOWN",
                        hal::pins::kBtnB, b ? "up" : "DOWN",
                        hal::pins::kBtnC, c ? "up" : "DOWN");
            last_a = a;
            last_b = b;
            last_c = c;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    std::printf("done. If the physical positions do not match top/middle/bottom,\n");
    std::printf("edit kLogicalToPhysical in main/hal/hal_input.cpp.\n");
    return 0;
}

int cmdVol(int argc, char** argv) {
    if (argc < 2) {
        std::printf("volume = %u/255\n", hal::audio::volume());
        return 0;
    }
    const int v = std::atoi(argv[1]);
    if (v < 0 || v > 255) {
        std::printf("usage: vol <0-255>\n");
        return 1;
    }
    hal::audio::setVolume(static_cast<uint8_t>(v));
    std::printf("volume = %u/255\n", hal::audio::volume());
    return 0;
}

int cmdBeep(int, char**) {
    hal::audio::chirp();
    std::printf("chirped at volume %u\n", hal::audio::volume());
    return 0;
}

int cmdSd(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "remount") == 0) {
        const esp_err_t err = hal::sd::remount();
        std::printf("remount: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    hal::sd::dumpInfo();
    std::printf("card detect (PMIC): %s\n",
                hal::power::sdCardInserted() ? "present" : "absent");
    std::printf("(use `sd remount` after swapping cards)\n");
    return 0;
}

int cmdI2c(int, char**) {
    boot::i2cScan();
    return 0;
}

int cmdEnv(int, char**) {
    hal::sensors::Reading r{};
    if (hal::sensors::read(&r)) {
        std::printf("SHT40: %.2f C, %.1f%% RH\n", r.temperature_c, r.humidity_pct);
    } else {
        std::printf("SHT40 read failed\n");
    }
    return 0;
}

int cmdDeck(int, char**) {
    if (!content::loaded()) {
        std::printf("deck NOT loaded\n");
        return 1;
    }
    std::printf("%s\n", content::stats());
    if (content::droppedCount() > 0) {
        std::printf("WARNING: %u card(s) dropped for missing assets\n",
                    (unsigned)content::droppedCount());
    }
    for (char l = 'A'; l <= 'Z'; ++l) {
        const int n = content::cardsForLetter(l);
        std::printf("  %c: %d %s\n", l, n, n == 0 ? "  <-- EMPTY" : "");
    }
    return 0;
}

int cmdReboot(int, char**) {
    std::printf("rebooting...\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

void reg(const char* cmd, const char* help, esp_console_cmd_func_t fn) {
    const esp_console_cmd_t c = {
        .command = cmd,
        .help = help,
        .hint = nullptr,
        .func = fn,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

}  // namespace

esp_err_t start() {
    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "phonics>";
    repl_cfg.max_cmdline_length = 128;
    repl_cfg.task_stack_size = 8192;

    esp_console_dev_usb_serial_jtag_config_t dev_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "REPL init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_console_register_help_command());

    reg("stat", "Full status dump: uptime, deck, sensors, RTC, SD, heap", cmdStat);
    reg("next", "Show the next card (picture + word + narration)", cmdNext);
    reg("letter", "Jump to the first card of the next letter", cmdLetter);
    reg("again", "Replay the current narration (no panel refresh)", cmdAgain);
    reg("repaint", "Force a full repaint (updates the clock)", cmdRepaint);
    reg("card", "card <A-Z> [index] -- show a specific card", cmdCard);
    reg("time", "time YYYY-MM-DD HH:MM:SS -- set the RTC", cmdTime);
    reg("btn", "Watch raw button GPIOs for 8s to identify top/middle/bottom", cmdBtn);
    reg("vol", "vol <0-255> -- get/set speaker volume", cmdVol);
    reg("beep", "Test the speaker", cmdBeep);
    reg("sd", "microSD info; `sd remount` to re-mount after a swap", cmdSd);
    reg("i2c", "Scan the internal I2C bus", cmdI2c);
    reg("env", "Read the SHT40 temperature/humidity now", cmdEnv);
    reg("deck", "Per-letter card counts", cmdDeck);
    reg("reboot", "Restart the device", cmdReboot);

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "REPL start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(kTag, "console ready -- type `help`");
    return ESP_OK;
}

}  // namespace app::console
