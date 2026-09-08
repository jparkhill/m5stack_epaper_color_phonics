/*
 * Phonics Cards -- a letter-sounds flashcard device for kids.
 * M5Stack PaperColor (SKU C151), ESP32-S3R8, 400x600 Spectra 6 e-paper.
 *
 * Boot is deliberately staged and loudly traced (see boot/boot_trace.h): a
 * cold start should be diagnosable from a single serial paste. A failing
 * stage degrades rather than aborts, so the device still comes up far enough
 * to answer the console.
 */
#include "app/app.h"
#include "app/console_cmds.h"
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

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

namespace {

constexpr const char* kTag = "main";
constexpr const char* kManifestPath = "/sd/phonics/manifest.json";

// esp_console's REPL takes ownership of stdin/stdout. Set false to get
// unmediated app-task logs when diagnosing something the console itself
// might be hiding.
constexpr bool kEnableConsole = true;

void initNvs() {
    boot::stage("nvs");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(kTag, "NVS needs erasing (%s); reinitialising", esp_err_to_name(err));
        if (nvs_flash_erase() == ESP_OK) err = nvs_flash_init();
    }
    if (err == ESP_OK) {
        boot::ok();
    } else {
        boot::fail(err, "nvs_flash_init");
    }
}

}  // namespace

extern "C" void app_main(void) {
    boot::begin();

    // --- Display first: M5.begin() brings up the internal I2C bus, the PMIC
    // rails and the panel, which everything else depends on. ---------------
    boot::stage("display");
    esp_err_t err = hal::display::init();
    if (err == ESP_OK) {
        boot::ok("%dx%d Spectra 6", static_cast<int>(hal::display::gfx().width()),
                 static_cast<int>(hal::display::gfx().height()));
    } else {
        boot::fail(err, "panel init");
        // Without a panel there is nothing to show, but keep going: the
        // console still works and can tell us why.
    }

    boot::stage("i2c scan");
    boot::i2cScan();
    boot::ok();

    initNvs();

    boot::stage("pmic");
    err = hal::power::init();
    if (err == ESP_OK) {
        // Start the LED animation NOW, not after the first card.
        //
        // Boot with a full SD deck takes ~25s (deck load plus a ~16s panel
        // refresh) and the e-paper keeps showing the PREVIOUS card the whole
        // time, because it is bistable. So a freshly woken device looks
        // completely dead, which is very easy to read as "it did not turn
        // on" -- and did get read that way. The LEDs are the only instant
        // feedback this hardware has, so they come up as soon as the PMIC
        // does.
        hal::power::ledRainbowStart();
        boot::ok("card detect %s",
                 hal::power::sdCardInserted() ? "present" : "absent");
    } else {
        boot::fail(err, "M5PM1 at 0x6E");
    }

    boot::stage("rtc");
    err = hal::rtc::init();
    if (err == ESP_OK) {
        boot::ok(hal::rtc::timeIsSuspect() ? "RX8130 (time is a guess)" : "RX8130");
    } else {
        boot::fail(err, "RX8130 at 0x32");
    }

    boot::stage("sensors");
    err = hal::sensors::init();
    if (err == ESP_OK) {
        hal::sensors::refresh();
        const auto& r = hal::sensors::last();
        boot::ok("%.1fC %.0f%%RH", r.temperature_c, r.humidity_pct);
    } else {
        boot::fail(err, "SHT40 at 0x44");
    }

    boot::stage("speaker");
    err = hal::audio::init();
    if (err == ESP_OK) {
        boot::ok("I2S %luHz", (unsigned long)hal::pins::kSpkSampleRate);
    } else {
        boot::fail(err, "I2S speaker");
    }

    boot::stage("buttons");
    hal::input::init();
    boot::ok("3 buttons");

    boot::stage("microSD");
    err = hal::sd::init();
    if (err == ESP_OK) {
        hal::sd::dumpInfo();
        uint64_t total_mb = 0, free_mb = 0;
        hal::sd::usage(&total_mb, &free_mb);
        boot::ok("%llu MB, %lu kHz", total_mb, (unsigned long)hal::sd::clockKhz());
    } else {
        boot::fail(err, "mount %s", hal::sd::kMountPoint);
    }

    boot::stage("deck");
    // Register the firmware's built-in cards first: the deck is then never
    // empty, so the device stays usable (and debuggable) with no SD card.
    err = content::begin();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "built-in deck init failed: %s", esp_err_to_name(err));
    }
    if (hal::sd::mounted()) {
        err = content::load(kManifestPath, /*verify_assets=*/true);
        if (err == ESP_OK) {
            boot::ok("%s", content::stats());
        } else {
            boot::fail(err, "load %s", kManifestPath);
        }
    } else {
        boot::skip("no microSD -- %u built-in card(s) only",
                   (unsigned)content::cardCount());
    }

    boot::stage("app");
    err = app::init();
    if (err == ESP_OK) {
        boot::ok();
    } else {
        boot::fail(err, "app init");
    }

    boot::heapReport("after init");
    boot::summary();

    boot::stage("console");
    if (!kEnableConsole) {
        boot::skip("disabled for log diagnosis");
    } else if (app::console::start() == ESP_OK) {
        boot::ok();
    } else {
        boot::fail(ESP_FAIL, "REPL");
    }

    app::run();   // never returns
}
