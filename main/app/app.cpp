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

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
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
uint32_t s_cards_shown = 0;

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
    const uint32_t ms = s_screen.present(hal::display::RefreshMode::kImage);
    s_last_clock_refresh_us = esp_timer_get_time();

    if (card != nullptr) {
        ++s_cards_shown;
        ESP_LOGI(kTag, "card #%lu: %-10s letter=%c span=[%d,%d] (refresh %lums)",
                 (unsigned long)s_cards_shown, card->display, card->letter,
                 card->span_start, card->span_len, (unsigned long)ms);
    }

    if (speak && card != nullptr) {
        hal::power::ledSet(hal::power::Led::kSpeaking);
        const bool spoke =
            card->isBuiltin()
                ? hal::audio::playWavMemoryBlocking(card->audio_data,
                                                    card->audio_len, card->display)
                : hal::audio::playWavFileBlocking(card->audio);
        if (!spoke) {
            ESP_LOGW(kTag, "narration failed for %s", card->id);
        }
    }
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
    if (c->isBuiltin()) {
        hal::audio::playWavMemoryBlocking(c->audio_data, c->audio_len, c->display);
    } else {
        hal::audio::playWavFileBlocking(c->audio);
    }
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
    boot::heapReport("now");
}

void handle(const Message& m) {
    switch (m.req) {
        case Req::kNextCard:
            if (s_deck_ok) showCard(content::advance(), true);
            break;
        case Req::kNextLetter:
            if (s_deck_ok) showCard(content::advanceLetter(), true);
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
    }
}

}  // namespace

esp_err_t init() {
    s_queue = xQueueCreate(8, sizeof(Message));
    if (s_queue == nullptr) return ESP_ERR_NO_MEM;

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
        s_screen.setFooterHint("check the serial console for details");
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

    ESP_LOGI(kTag, "ready. Either cycle button = new word; hold = next letter; "
                   "third button = say it again");

    for (;;) {
        hal::input::update();

        // Either cycle button shows the next card -- see hal_input.h for why
        // both do the same thing. A long press jumps a whole letter.
        if (hal::input::wasHeldFor(hal::input::Button::kCycleA, 700) ||
            hal::input::wasHeldFor(hal::input::Button::kCycleB, 700)) {
            ESP_LOGI(kTag, "long press -> next letter");
            hal::audio::chirp();
            requestNextLetter();
        } else if (hal::input::wasPressed(hal::input::Button::kCycleA) ||
                   hal::input::wasPressed(hal::input::Button::kCycleB)) {
            ESP_LOGI(kTag, "press -> next card");
            hal::audio::chirp();
            requestNextCard();
        } else if (hal::input::wasPressed(hal::input::Button::kExtra)) {
            ESP_LOGI(kTag, "press -> replay narration");
            requestReplay();
        }

        Message m{};
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(20)) == pdTRUE) {
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
void requestReplay()     { post(Message{Req::kReplay, 0, 0}); }
void requestRepaint()    { post(Message{Req::kRepaint, 0, 0}); }
void requestStatusDump() { post(Message{Req::kStatusDump, 0, 0}); }

void requestCard(char letter, int nth) {
    post(Message{Req::kSelectCard, letter, static_cast<int8_t>(nth)});
}

}  // namespace app
