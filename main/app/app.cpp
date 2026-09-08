#include "app/app.h"
#include "boot/boot_trace.h"
#include "content/deck.h"
#include "hal/hal_audio.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "hal/hal_power.h"
#include "hal/hal_rtc.h"
#include "hal/hal_sd.h"
#include "hal/hal_sensors.h"
#include "ui/screen.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <driver/usb_serial_jtag.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <M5Unified.hpp>

namespace app {
namespace {

constexpr const char* kTag = "app";

enum class Req : uint8_t {
    kNextCard,
    kNextLetter,
    kReplay,
    kRepaint,
    kSelectCard,
    kStatusDump,
    kSleep,
    kNextLetterInWord,
};

struct Message {
    Req req;
    char letter;
    int8_t nth;
};

ui::Screen s_screen;
QueueHandle_t s_queue = nullptr;
bool s_deck_ok = false;
int64_t s_last_clock_refresh_us = 0;
int64_t s_last_sensor_us = 0;
int64_t s_last_activity_us = 0;
uint32_t s_cards_shown = 0;
bool s_sleep_enabled = true;

constexpr const char* kNvsNamespace = "phonics";
constexpr const char* kNvsSleepKey = "sleep_en";

void loadSleepSetting() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 1;
    if (nvs_get_u8(h, kNvsSleepKey, &v) == ESP_OK) s_sleep_enabled = (v != 0);
    nvs_close(h);
}

void saveSleepSetting() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, kNvsSleepKey, s_sleep_enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

/// Anything the user did. Resets the idle timeout.
void noteActivity() { s_last_activity_us = esp_timer_get_time(); }

void goToSleep(const char* why) {
    ESP_LOGW(kTag, "sleeping: %s", why);
    ESP_LOGW(kTag, "the current card stays on screen -- e-paper holds its "
                   "image with no power");

    // A descending pair so it is obvious the device chose to sleep rather
    // than crashed.
    hal::audio::tone(880.0f, 120);
    hal::audio::tone(560.0f, 160);

    hal::power::ledRainbowStop();
    hal::power::ledSet(hal::power::Led::kIdle);

    // Put the panel into its own sleep state before the rails drop, rather
    // than cutting power mid-scan.
    hal::display::gfx().sleep();

    hal::power::powerOff();

    // Only reached if the PMIC refused (e.g. held up by USB power).
    ESP_LOGW(kTag, "power off did not take effect; resuming");
    noteActivity();
    hal::display::gfx().wakeup();
    hal::power::ledRainbowStart();
}

void post(const Message& m) {
    if (s_queue == nullptr) return;
    if (xQueueSend(s_queue, &m, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(kTag, "request queue full, dropping request");
    }
}

/// Repaint everything and push one panel refresh. Optionally narrate after.
void showCard(const content::Card* card, bool speak) {
    hal::power::ledSet(hal::power::Led::kBusy);

    // Refresh the cheap stuff first so the status bar is current in this frame.
    hal::sensors::refresh();
    hal::rtc::syncSystemClock();
    s_last_sensor_us = esp_timer_get_time();

    s_screen.cardView().setCard(card);
    {
        const content::Focus f = content::currentFocus();
        s_screen.cardView().setFocus(f.start, f.len);
    }

    // Compose the frame in PSRAM (~157ms) but do NOT push it yet.
    s_screen.render();

    // Kick the narration off on the audio task now. A panel refresh blocks
    // this task for ~16s, so playing the sound afterwards means the child
    // waits in silence and then hears the word. Starting it here lets the
    // sound run WHILE the picture develops, which removes nearly all of the
    // perceived latency.
    //
    // Any SD read has to happen before the refresh starts, because the card
    // and the panel share SPI2 -- hence preload-then-play rather than
    // streaming during the refresh.
    // Narration is TWO clips played back to back: the shared per-letter clip
    // for whichever letter is in focus, then the word clip. That split is
    // what makes "step through every letter of the word" affordable -- see
    // tools/phonics_data.py.
    //
    // Both reads must finish BEFORE the refresh starts, because the SD card
    // and the panel share SPI2.
    bool narrating = false;
    if (speak && card != nullptr) {
        const content::Focus focus = content::currentFocus();
        if (card->isBuiltin()) {
            narrating = hal::audio::playMemoryAsync(card->audio_data,
                                                    card->audio_len,
                                                    card->display);
        } else {
            const char* letter_clip = content::letterAudioPath(focus.letter);
            const bool have_letter =
                letter_clip != nullptr &&
                hal::audio::preloadWavFile(hal::audio::Slot::kLetter, letter_clip);
            const bool have_word =
                hal::audio::preloadWavFile(hal::audio::Slot::kWord, card->audio);

            if (have_letter) {
                char lbl[8];
                std::snprintf(lbl, sizeof(lbl), "%c", focus.letter);
                narrating |= hal::audio::playSlotAsync(hal::audio::Slot::kLetter, lbl);
            }
            if (have_word) {
                narrating |= hal::audio::playSlotAsync(hal::audio::Slot::kWord,
                                                       card->display);
            }
        }
        if (!narrating) {
            ESP_LOGW(kTag, "narration failed for %s", card->id);
        } else {
            hal::power::ledSet(hal::power::Led::kSpeaking);
        }
    }

    const uint32_t ms = hal::display::present(hal::display::RefreshMode::kImage);
    s_last_clock_refresh_us = esp_timer_get_time();

    if (card != nullptr) {
        ++s_cards_shown;
        ESP_LOGI(kTag, "card #%lu: %-10s letter=%c span=[%d,%d] (refresh %lums%s)",
                 (unsigned long)s_cards_shown, card->display, card->letter,
                 card->span_start, card->span_len, (unsigned long)ms,
                 narrating ? ", narrated during refresh" : "");
    }

    // The clip is almost always finished by now; wait for the tail if not.
    if (narrating) hal::audio::waitIdle(20000);
    hal::power::ledSet(hal::power::Led::kIdle);
}

void repaintOnly() {
    hal::power::ledSet(hal::power::Led::kBusy);
    hal::sensors::refresh();
    hal::rtc::syncSystemClock();
    s_screen.present(hal::display::RefreshMode::kImage);
    s_last_clock_refresh_us = esp_timer_get_time();
    s_last_sensor_us = esp_timer_get_time();
    hal::power::ledSet(hal::power::Led::kIdle);
}

void replayAudio() {
    const content::Card* c = content::current();
    if (c == nullptr) {
        ESP_LOGW(kTag, "nothing to replay");
        return;
    }
    ESP_LOGI(kTag, "replaying %s (no panel refresh)", c->id);
    hal::power::ledSet(hal::power::Led::kSpeaking);
    const content::Focus focus = content::currentFocus();
    if (c->isBuiltin()) {
        hal::audio::playMemoryAsync(c->audio_data, c->audio_len, c->display);
    } else {
        const char* letter_clip = content::letterAudioPath(focus.letter);
        if (letter_clip != nullptr &&
            hal::audio::preloadWavFile(hal::audio::Slot::kLetter, letter_clip)) {
            char lbl[8];
            std::snprintf(lbl, sizeof(lbl), "%c", focus.letter);
            hal::audio::playSlotAsync(hal::audio::Slot::kLetter, lbl);
        }
        if (hal::audio::preloadWavFile(hal::audio::Slot::kWord, c->audio)) {
            hal::audio::playSlotAsync(hal::audio::Slot::kWord, c->display);
        }
    }
    hal::audio::waitIdle(20000);
    hal::power::ledSet(hal::power::Led::kIdle);
}

void dumpStatus() {
    // ESP_LOG, not printf: once esp_console owns stdout, printf from this task
    // is buffered and never reaches the wire, so `stat` silently produced
    // nothing. The log path always writes through.
    ESP_LOGI(kTag, "--- status ---");
    ESP_LOGI(kTag, "uptime          : %.1f s", esp_timer_get_time() / 1e6);
    ESP_LOGI(kTag, "cards shown     : %lu", (unsigned long)s_cards_shown);
    ESP_LOGI(kTag, "panel refreshes : %lu (last %lu ms)",
             (unsigned long)hal::display::refreshCount(),
             (unsigned long)hal::display::lastRefreshMs());

    const content::Card* c = content::current();
    ESP_LOGI(kTag, "current card    : %s", c ? c->display : "(none)");
    if (c != nullptr) {
        ESP_LOGI(kTag, "  id    : %s%s", c->id, c->isBuiltin() ? "  [built-in]" : "");
        ESP_LOGI(kTag, "  image : %s", c->image);
        ESP_LOGI(kTag, "  audio : %s", c->audio);
        ESP_LOGI(kTag, "  span  : [%d,%d]", c->span_start, c->span_len);
    }

    ESP_LOGI(kTag, "deck            : %s",
             content::loaded() ? content::stats() : "NOT LOADED");

    const auto& env = hal::sensors::last();
    if (env.valid) {
        ESP_LOGI(kTag, "environment     : %.2f C, %.1f%% RH", env.temperature_c,
                 env.humidity_pct);
    } else {
        ESP_LOGI(kTag, "environment     : no valid reading");
    }

    hal::rtc::DateTime dt{};
    if (hal::rtc::get(&dt)) {
        ESP_LOGI(kTag, "rtc             : %04u-%02u-%02u %02u:%02u:%02u%s", dt.year,
                 dt.month, dt.day, dt.hour, dt.minute, dt.second,
                 hal::rtc::timeIsSuspect() ? "  (SUSPECT - run `time`)" : "");
    } else {
        ESP_LOGI(kTag, "rtc             : unavailable");
    }

    uint64_t total_mb = 0, free_mb = 0;
    if (hal::sd::usage(&total_mb, &free_mb)) {
        ESP_LOGI(kTag, "microSD         : %llu MB total, %llu MB free, %lu kHz",
                 total_mb, free_mb, (unsigned long)hal::sd::clockKhz());
    } else {
        ESP_LOGI(kTag, "microSD         : not mounted");
    }

    ESP_LOGI(kTag, "volume          : %u/255", hal::audio::volume());
    ESP_LOGI(kTag, "buttons         : %s", hal::input::rawSnapshot());
    if (s_sleep_enabled) {
        ESP_LOGI(kTag, "sleeps in       : %lu s (idle timeout %lu min)",
                 (unsigned long)secondsUntilSleep(),
                 (unsigned long)(kIdleSleepSec / 60));
    } else {
        ESP_LOGI(kTag, "sleeps in       : never (auto power-off disabled)");
    }
    boot::heapReport("now");
}

void handle(const Message& m) {
    switch (m.req) {
        case Req::kNextCard:
            if (s_deck_ok) showCard(content::advanceRandom(), true);
            break;
        case Req::kNextLetter:
            if (s_deck_ok) showCard(content::advanceLetter(), true);
            break;
        case Req::kNextLetterInWord:
            if (s_deck_ok) {
                const content::Focus f = content::advanceFocus();
                ESP_LOGI(kTag, "focus -> '%c' at offset %d", f.letter, f.start);
                showCard(content::current(), true);
            }
            break;
        case Req::kReplay:
            replayAudio();
            break;
        case Req::kRepaint:
            repaintOnly();
            break;
        case Req::kSelectCard:
            if (s_deck_ok) {
                const content::Card* c = content::selectLetter(m.letter, m.nth);
                if (c == nullptr) {
                    ESP_LOGW(kTag, "no card for letter '%c' index %d", m.letter,
                             m.nth);
                } else {
                    showCard(c, true);
                }
            }
            break;
        case Req::kStatusDump:
            dumpStatus();
            break;
        case Req::kSleep:
            goToSleep("requested over the console");
            break;
    }
}

}  // namespace

esp_err_t init() {
    s_queue = xQueueCreate(8, sizeof(Message));
    if (s_queue == nullptr) return ESP_ERR_NO_MEM;

    loadSleepSetting();
    s_screen.init();
    s_screen.describe();

    s_deck_ok = content::loaded() && content::cardCount() > 0;

    if (!s_deck_ok) {
        // Put the failure on the screen, not just in the log: the device is
        // meant to be usable away from a laptop.
        const char* why = "no cards loaded";
        if (!hal::sd::mounted()) why = "microSD not mounted";
        s_screen.cardView().setPlaceholder(why, "run tools/provision_sd.sh");
        s_screen.cardView().setCard(nullptr);
        ESP_LOGE(kTag, "starting in DIAGNOSTIC mode: %s", why);
    }
    return ESP_OK;
}

void run() {
    // First frame. Silent on boot -- a power cycle should not shout.
    if (s_deck_ok) {
        showCard(content::advance(), false);
        // Two rising notes: proves the codec + I2S path works without
        // narrating a whole card on every power-up.
        hal::audio::bootChime();
    } else {
        hal::power::ledSet(hal::power::Led::kError);
        s_screen.present(hal::display::RefreshMode::kTextOnly);
    }

    // Attention-holder: keeps moving through the blocking panel refresh.
    hal::power::ledRainbowStart();

    noteActivity();
    ESP_LOGI(kTag, "ready. upper side = next card | lower side = repeat | "
                   "top = next letter in the word");
    if (s_sleep_enabled) {
        ESP_LOGI(kTag, "auto power-off after %lu min of no activity%s",
                 (unsigned long)(kIdleSleepSec / 60),
                 kSleepWhileUsbConnected ? "" : " (deferred while USB attached)");
        ESP_LOGI(kTag, "waking needs a LONG press of PWR_KEY (~2-4s); a short "
                       "click is a reset. `nosleep` disables the timeout.");
    } else {
        ESP_LOGW(kTag, "auto power-off is DISABLED (persisted); `autosleep` "
                       "re-enables it");
    }

    for (;;) {
        hal::input::update();

        // Physical mapping, confirmed on hardware (see hal_input.h):
        //   G10 upper side -> next card
        //   G9  lower side -> repeat the current letter + word
        //   G1  top (middle of the title bar) -> next letter IN THE WORD
        //   PWR_KEY        -> power/wake, handled by the PMIC
        if (hal::input::wasPressed(hal::input::Button::kNextCard)) {
            ESP_LOGI(kTag, "upper side -> next card");
            hal::audio::chirp();
            requestNextCard();
        } else if (hal::input::wasPressed(hal::input::Button::kRepeat)) {
            ESP_LOGI(kTag, "lower side -> repeat");
            requestReplay();
        } else if (hal::input::wasPressed(hal::input::Button::kLetterInWord)) {
            ESP_LOGI(kTag, "top -> next letter in the word");
            hal::audio::chirp();
            requestNextLetterInWord();
        }

        Message m{};
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(20)) == pdTRUE) {
            noteActivity();
            handle(m);
            // Drain any presses that piled up during the ~10s refresh so one
            // impatient child does not queue up ten refreshes.
            hal::input::update();
            xQueueReset(s_queue);
            continue;
        }

        const int64_t now = esp_timer_get_time();

        if (now - s_last_sensor_us > static_cast<int64_t>(kSensorSampleSec) * 1000000) {
            hal::sensors::refresh();
            s_last_sensor_us = now;
        }

        if (s_sleep_enabled &&
            now - s_last_activity_us >
                static_cast<int64_t>(kIdleSleepSec) * 1000000) {
            if (!kSleepWhileUsbConnected && usb_serial_jtag_is_connected()) {
                // Do not yank the serial port out from under a developer.
                ESP_LOGD(kTag, "idle timeout reached but USB is attached; "
                               "deferring sleep");
                noteActivity();
            } else {
                goToSleep("no activity for 15 minutes");
                continue;
            }
        }

        if (s_deck_ok &&
            now - s_last_clock_refresh_us >
                static_cast<int64_t>(kIdleClockRefreshSec) * 1000000) {
            ESP_LOGD(kTag, "idle clock repaint");
            repaintOnly();
        }
    }
}

void requestNextCard()   { post(Message{Req::kNextCard, 0, 0}); }
void requestNextLetter() { post(Message{Req::kNextLetter, 0, 0}); }
void requestNextLetterInWord() { post(Message{Req::kNextLetterInWord, 0, 0}); }
void requestReplay()     { post(Message{Req::kReplay, 0, 0}); }
void requestRepaint()    { post(Message{Req::kRepaint, 0, 0}); }
void requestStatusDump() { post(Message{Req::kStatusDump, 0, 0}); }
void requestSleep()      { post(Message{Req::kSleep, 0, 0}); }

void setIdleSleepEnabled(bool enabled) {
    s_sleep_enabled = enabled;
    saveSleepSetting();
    noteActivity();
    ESP_LOGW(kTag, "auto power-off %s (saved to NVS)",
             enabled ? "ENABLED" : "DISABLED");
}

bool idleSleepEnabled() { return s_sleep_enabled; }

uint32_t secondsUntilSleep() {
    const int64_t elapsed = esp_timer_get_time() - s_last_activity_us;
    const int64_t budget = static_cast<int64_t>(kIdleSleepSec) * 1000000;
    if (elapsed >= budget) return 0;
    return static_cast<uint32_t>((budget - elapsed) / 1000000);
}

void requestCard(char letter, int nth) {
    post(Message{Req::kSelectCard, letter, static_cast<int8_t>(nth)});
}

}  // namespace app
