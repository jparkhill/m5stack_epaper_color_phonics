#include "boot/power_log.h"
#include "hal/hal_pins.h"

#include <cstdio>
#include <cstring>

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>

namespace boot::plog {
namespace {

constexpr const char* kTag = "plog";
constexpr uint32_t kMagic = 0x504C4F47;   // "PLOG"
constexpr uint32_t kCapacity = 128;

struct Entry {
    uint32_t uptime_ms;   // within the boot that recorded it
    uint32_t detail;      // event-specific
    uint16_t boot_id;     // increments every boot, so sessions are separable
    uint8_t event;
    uint8_t reserved;
};   // 12 bytes; padded to 16 by the array

// RTC_NOINIT_ATTR: retained across deep sleep AND software reset, and NOT
// zeroed at startup. Hence the magic word to detect a genuine power loss.
RTC_NOINIT_ATTR uint32_t s_magic;
RTC_NOINIT_ATTR uint32_t s_write_pos;
RTC_NOINIT_ATTR uint32_t s_total;
RTC_NOINIT_ATTR uint16_t s_boot_id;
RTC_NOINIT_ATTR Entry s_ring[kCapacity];

bool s_carried_over = false;

void ensureInit() {
    if (s_magic == kMagic) return;
    s_magic = kMagic;
    s_write_pos = 0;
    s_total = 0;
    s_boot_id = 0;
    std::memset(s_ring, 0, sizeof(s_ring));
}

const char* eventName(uint8_t e) {
    switch (static_cast<Event>(e)) {
        case Event::kBoot:        return "BOOT";
        case Event::kSleepEnter:  return "SLEEP-ENTER";
        case Event::kWake:        return "WAKE";
        case Event::kRailsDown:   return "RAILS-DOWN";
        case Event::kPowerOffCmd: return "POWEROFF-CMD";
        case Event::kReady:       return "READY";
        case Event::kSdMounted:   return "SD-MOUNTED";
        case Event::kSdFailed:    return "SD-FAILED";
        case Event::kDeckLoaded:  return "DECK-LOADED";
        case Event::kPresentStart: return "PRESENT-START";
        case Event::kPresentDone:  return "PRESENT-DONE";
        case Event::kChimeDone:    return "CHIME-DONE";
        case Event::kBattery:      return "BATTERY";
        default:                  return "?";
    }
}

const char* resetName(uint32_t r) {
    switch (static_cast<esp_reset_reason_t>(r)) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_USB:       return "USB";
        case ESP_RST_JTAG:      return "JTAG";
        default:                return "UNKNOWN";
    }
}

const char* wakeName(uint32_t c) {
    switch (static_cast<esp_sleep_source_t>(c)) {
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "none/undefined";
        case ESP_SLEEP_WAKEUP_EXT0:      return "EXT0";
        case ESP_SLEEP_WAKEUP_EXT1:      return "EXT1 (button)";
        case ESP_SLEEP_WAKEUP_TIMER:     return "TIMER";
        case ESP_SLEEP_WAKEUP_GPIO:      return "GPIO";
        case ESP_SLEEP_WAKEUP_UART:      return "UART";
        case ESP_SLEEP_WAKEUP_ULP:       return "ULP";
        default:                         return "other";
    }
}

}  // namespace

void record(Event ev, uint32_t detail) {
    ensureInit();
    Entry& e = s_ring[s_write_pos % kCapacity];
    e.uptime_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    e.detail = detail;
    e.boot_id = s_boot_id;
    e.event = static_cast<uint8_t>(ev);
    e.reserved = 0;
    s_write_pos = (s_write_pos + 1) % kCapacity;
    if (s_total < 0xFFFFFFFFu) ++s_total;
}

void begin() {
    s_carried_over = (s_magic == kMagic);
    ensureInit();
    ++s_boot_id;

    const esp_reset_reason_t rr = esp_reset_reason();
    record(Event::kBoot, static_cast<uint32_t>(rr));

    if (rr == ESP_RST_DEEPSLEEP) {
        const esp_sleep_source_t cause = esp_sleep_get_wakeup_cause();
        record(Event::kWake, static_cast<uint32_t>(cause));
        if (cause == ESP_SLEEP_WAKEUP_EXT1) {
            // Which pin actually woke us -- confirms the button mapping.
            const uint64_t mask = esp_sleep_get_ext1_wakeup_status();
            ESP_LOGW(kTag, "woke from deep sleep via EXT1, GPIO mask 0x%llx",
                     mask);
            for (int pin = 0; pin < 22; ++pin) {
                if (mask & (1ULL << pin)) {
                    ESP_LOGW(kTag, "  -> GPIO%d pulled the wake line", pin);
                }
            }
        } else {
            ESP_LOGW(kTag, "woke from deep sleep, cause=%s", wakeName(cause));
        }
    }

    ESP_LOGI(kTag, "power log: boot #%u, reset=%s, %u events retained%s",
             (unsigned)s_boot_id, resetName(static_cast<uint32_t>(rr)),
             (unsigned)(s_total < kCapacity ? s_total : kCapacity),
             s_carried_over ? "" : " (fresh -- RTC memory was lost, so the "
                                   "rails really dropped)");
}

void dump() {
    ensureInit();
    const uint32_t have = (s_total < kCapacity) ? s_total : kCapacity;
    std::printf("\n--- power event log (%u of max %u, oldest first) ---\n",
                (unsigned)have, (unsigned)kCapacity);
    std::printf("this boot is #%u; RTC memory %s\n", (unsigned)s_boot_id,
                s_carried_over ? "carried over (no power loss)"
                               : "was reinitialised (power was removed)");
    if (have == 0) {
        std::printf("(empty)\n");
        return;
    }
    // Oldest first: start just past the write cursor once wrapped.
    const uint32_t start = (s_total <= kCapacity)
                               ? 0
                               : (s_write_pos % kCapacity);
    std::printf("%5s %6s %10s  %-13s %s\n", "idx", "boot", "uptime", "event",
                "detail");
    for (uint32_t i = 0; i < have; ++i) {
        const Entry& e = s_ring[(start + i) % kCapacity];
        char detail[48] = "";
        switch (static_cast<Event>(e.event)) {
            case Event::kBoot:
                std::snprintf(detail, sizeof(detail), "reset=%s",
                              resetName(e.detail));
                break;
            case Event::kWake:
                std::snprintf(detail, sizeof(detail), "cause=%s",
                              wakeName(e.detail));
                break;
            case Event::kSleepEnter:
                std::snprintf(detail, sizeof(detail), "wake mask=0x%lx",
                              (unsigned long)e.detail);
                break;
            case Event::kDeckLoaded:
                std::snprintf(detail, sizeof(detail), "%lu cards",
                              (unsigned long)e.detail);
                break;
            case Event::kPresentDone:
                std::snprintf(detail, sizeof(detail), "%lu ms refresh",
                              (unsigned long)e.detail);
                break;
            case Event::kPresentStart:
            case Event::kBattery:
                std::snprintf(detail, sizeof(detail), "%lu mV",
                              (unsigned long)e.detail);
                break;
            case Event::kRailsDown:
                std::snprintf(detail, sizeof(detail), "PWR_CFG=0x%02lx",
                              (unsigned long)e.detail);
                break;
            default:
                if (e.detail) {
                    std::snprintf(detail, sizeof(detail), "%lu",
                                  (unsigned long)e.detail);
                }
                break;
        }
        std::printf("%5lu %6u %8lu ms  %-13s %s\n", (unsigned long)i,
                    (unsigned)e.boot_id, (unsigned long)e.uptime_ms,
                    eventName(e.event), detail);
    }
    std::printf("--- end ---\n\n");
    std::fflush(stdout);
}

void clear() {
    s_magic = 0;
    ensureInit();
    ESP_LOGW(kTag, "power event log cleared");
}

uint32_t count() { return s_total; }
bool carriedOver() { return s_carried_over; }

}  // namespace boot::plog
