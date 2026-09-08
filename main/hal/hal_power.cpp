#include "hal/hal_power.h"
#include "hal/hal_pins.h"

#include <cmath>
#include <cstdio>

#include <driver/rtc_io.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <M5Unified.hpp>

namespace hal::power {
namespace {

constexpr const char* kTag = "power";
constexpr uint32_t kFreq = 100000;

// ---------------------------------------------------------------------------
// M5PM1 GPIO registers.
//
// We talk to the PMIC directly rather than through the m5stack/m5pm1
// component. That component's begin(m5::I2C_Class*) overload is gated on
// __has_include(<utility/I2C_Class.hpp>), which is false while the component
// itself compiles -- so the symbol is declared but never emitted and the link
// fails. Its other begin() overloads would install a *second* I2C driver on
// GPIO2/3, fighting the bus M5Unified already owns. Five register writes are
// simpler and safer than either.
// ---------------------------------------------------------------------------
constexpr uint8_t kRegMode  = 0x10;  // [4:0] direction, 1=output
constexpr uint8_t kRegOut   = 0x11;  // [4:0] output level
constexpr uint8_t kRegIn    = 0x12;  // [4:0] input level (read-only)
constexpr uint8_t kRegDrv   = 0x13;  // [4:0] 1=open-drain, 0=push-pull
constexpr uint8_t kRegPupd0 = 0x14;  // 2 bits/pin for GPIO0..3, 01=pull-up
constexpr uint8_t kRegFunc0 = 0x16;  // 2 bits/pin for GPIO0..3, 00=plain GPIO
constexpr uint8_t kRegFunc1 = 0x17;  // [1:0] GPIO4, 00=plain GPIO

// System command register. [7:4] must be the key 0xA, [1:0] is the command:
// 00 no-op, 01 shutdown, 10 reboot, 11 download mode.
constexpr uint8_t kRegSysCmd = 0x0C;

// Battery voltage in mV, split across two registers: low byte then the high
// 4 bits. And the active power source, so we can tell charging from draining.
// Voltage registers are plain 16-bit LITTLE-ENDIAN millivolts, using the
// FULL high byte.
//
// The M5PM1 header comments claim "high 4 bits" for VBAT/VIN, and believing
// that produced nonsense: masking the high byte with 0x0F turned a healthy
// 4158 mV cell into "118 mV (~0%)" and had me confidently blaming a flat
// battery for a wake failure. A raw register dump settled it:
//     VBAT 3E 10 -> 0x103E = 4158 mV      (plausible Li-ion)
//     VIN  A0 13 -> 0x13A0 = 5024 mV      (USB 5V, exactly right)
//     VREF ED 0C -> 0x0CED = 3309 mV      (3.3V reference)
// Cross-checking against a value you already know (VIN on USB) is what
// exposed it; never trust a single unverified decode.
constexpr uint8_t kRegVrefL = 0x20;
constexpr uint8_t kRegVrefH = 0x21;
constexpr uint8_t kRegVbatL = 0x22;
constexpr uint8_t kRegVbatH = 0x23;
constexpr uint8_t kRegVinL  = 0x24;
constexpr uint8_t kRegVinH  = 0x25;

constexpr uint8_t kRegPwrSrc = 0x04;   // [2:0] 0=5VIN, 1=5VINOUT, 2=battery
constexpr uint8_t kRegWakeSrc = 0x05;  // [6:0] wake-source flags
constexpr uint8_t kRegPwrCfg  = 0x06;  // rail enables
constexpr uint8_t kRegBattLvp = 0x08;  // low-voltage protection threshold
constexpr uint8_t kRegI2cCfg  = 0x09;  // [3:0] SLP_TO: PMIC sleeps on I2C idle
constexpr uint8_t kSysCmdKey = 0xA0;
constexpr uint8_t kSysCmdShutdown = 0x01;

bool s_available = false;
bool s_led_ok = false;

TaskHandle_t s_rainbow_task = nullptr;
volatile bool s_rainbow_run = false;

/// Hue (0..1) to RGB. Full saturation/value; brightness is left to
/// LED_Class::setBrightness so the two knobs stay independent.
void hueToRgb(float h, uint8_t* r, uint8_t* g, uint8_t* b) {
    h -= std::floor(h);
    const float x = h * 6.0f;
    const int sector = static_cast<int>(x) % 6;
    const float f = x - std::floor(x);
    const uint8_t up = static_cast<uint8_t>(f * 255.0f);
    const uint8_t dn = static_cast<uint8_t>((1.0f - f) * 255.0f);
    switch (sector) {
        case 0: *r = 255; *g = up;  *b = 0;   break;
        case 1: *r = dn;  *g = 255; *b = 0;   break;
        case 2: *r = 0;   *g = 255; *b = up;  break;
        case 3: *r = 0;   *g = dn;  *b = 255; break;
        case 4: *r = up;  *g = 0;   *b = 255; break;
        default:*r = 255; *g = 0;   *b = dn;  break;
    }
}

void rainbowTask(void*) {
    float hue = 0.0f;
    while (s_rainbow_run) {
        uint8_t r0, g0, b0, r1, g1, b1;
        hueToRgb(hue, &r0, &g0, &b0);
        // Second LED a third of the wheel behind, so the pair sweeps rather
        // than blinking in unison.
        hueToRgb(hue + 0.33f, &r1, &g1, &b1);
        M5.Led.setColor(0, r0, g0, b0);
        M5.Led.setColor(1, r1, g1, b1);
        M5.Led.display();
        hue += 0.012f;                    // ~4s per full cycle at 50ms steps
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    M5.Led.setAllColor(0, 0, 0);
    M5.Led.display();
    s_rainbow_task = nullptr;
    vTaskDelete(nullptr);
}

bool rd(uint8_t reg, uint8_t* v) {
    return M5.In_I2C.readRegister(pins::kAddrPm1, reg, v, 1, kFreq);
}

bool wr(uint8_t reg, uint8_t v) {
    return M5.In_I2C.writeRegister(pins::kAddrPm1, reg, &v, 1, kFreq);
}

/// Read a 16-bit little-endian millivolt pair.
bool readMillivolts(uint8_t reg_l, uint8_t reg_h, uint16_t* out) {
    uint8_t lo = 0, hi = 0;
    if (!rd(reg_l, &lo) || !rd(reg_h, &hi)) return false;
    *out = static_cast<uint16_t>(lo | (hi << 8));
    return true;
}

/// Read-modify-write: clear `clear_mask`, then set `set_mask`.
bool rmw(uint8_t reg, uint8_t clear_mask, uint8_t set_mask) {
    uint8_t v = 0;
    if (!rd(reg, &v)) return false;
    v = static_cast<uint8_t>((v & ~clear_mask) | set_mask);
    return wr(reg, v);
}

}  // namespace

esp_err_t init() {
    if (!M5.In_I2C.scanID(pins::kAddrPm1, kFreq)) {
        ESP_LOGE(kTag, "M5PM1 not responding at 0x%02X", pins::kAddrPm1);
        s_available = false;
        return ESP_ERR_NOT_FOUND;
    }

    bool ok = true;

    // --- SD_DET_EN (PMIC GPIO4): plain GPIO, push-pull output, driven HIGH.
    // Card detect is gated behind this: without it SD_DEC floats and the card
    // always reads as absent. Easy to miss when porting.
    ok &= rmw(kRegFunc1, 0x03, 0x00);                                  // 00 = GPIO
    ok &= rmw(kRegDrv, 1u << pins::kPm1SdDetectEn, 0x00);              // push-pull
    ok &= rmw(kRegMode, 0x00, 1u << pins::kPm1SdDetectEn);             // output
    ok &= rmw(kRegOut, 0x00, 1u << pins::kPm1SdDetectEn);              // high

    // --- SD_DEC (PMIC GPIO1): plain GPIO input with a pull-up, active LOW.
    constexpr uint8_t kFunc1Shift = pins::kPm1SdDetect * 2;            // 2 bits/pin
    ok &= rmw(kRegFunc0, static_cast<uint8_t>(0x03u << kFunc1Shift), 0x00);
    ok &= rmw(kRegMode, 1u << pins::kPm1SdDetect, 0x00);               // input
    ok &= rmw(kRegPupd0, static_cast<uint8_t>(0x03u << kFunc1Shift),
              static_cast<uint8_t>(0x01u << kFunc1Shift));             // pull-up

    if (!ok) {
        ESP_LOGE(kTag, "PMIC GPIO configuration failed mid-sequence");
        s_available = false;
        return ESP_FAIL;
    }
    s_available = true;

    s_led_ok = M5.Led.isEnabled();
    if (s_led_ok) {
        M5.Led.setBrightness(40);
        ledSet(Led::kIdle);
        ESP_LOGI(kTag, "PMIC ok; RGB status LEDs on GPIO%d", pins::kRgbLed);
    } else {
        ESP_LOGW(kTag, "PMIC ok but no RGB LED instance; LED feedback disabled");
    }
    return ESP_OK;
}

bool available() { return s_available; }

bool sdCardInserted() {
    if (!s_available) return false;
    uint8_t v = 0;
    if (!rd(kRegIn, &v)) {
        ESP_LOGW(kTag, "could not read PMIC input register");
        return false;
    }
    // Active LOW: bit clear means a card is seated.
    return (v & (1u << pins::kPm1SdDetect)) == 0;
}

void ledRainbowStart() {
    if (!s_led_ok || s_rainbow_task != nullptr) return;
    s_rainbow_run = true;
    // Pinned to CPU1: CPU0 is blocked inside the panel refresh for seconds at
    // a time, and the whole point of this animation is to keep moving then.
    if (xTaskCreatePinnedToCore(rainbowTask, "led_rainbow", 3072, nullptr, 1,
                                &s_rainbow_task, 1) != pdPASS) {
        ESP_LOGW(kTag, "could not start the LED rainbow task");
        s_rainbow_run = false;
        s_rainbow_task = nullptr;
        return;
    }
    ESP_LOGI(kTag, "LED rainbow running on CPU1");
}

void ledRainbowStop() {
    s_rainbow_run = false;
    for (int i = 0; i < 40 && s_rainbow_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool ledRainbowRunning() { return s_rainbow_task != nullptr; }

Battery readBattery() {
    Battery b{};
    if (!s_available) return b;

    if (!readMillivolts(kRegVbatL, kRegVbatH, &b.millivolts)) return b;

    // PWR_SRC (0x04) does not match its documented 0/1/2 enum on this part --
    // it read 0x05, which is out of range. So infer external power from the
    // VIN rail instead, which decodes correctly and is unambiguous.
    uint16_t vin = 0;
    if (readMillivolts(kRegVinL, kRegVinH, &vin)) {
        b.charging = vin > 4300;
    }

    // A single-cell Li-ion is flat around 3.3V and full around 4.2V, and the
    // curve is far from linear -- a straight 3.3-4.2 map reads ~50% when the
    // cell is nearly empty. These breakpoints follow the discharge plateau so
    // the number degrades honestly.
    struct Point { uint16_t mv; uint8_t pct; };
    static constexpr Point kCurve[] = {
        {3300, 0}, {3600, 10}, {3700, 25}, {3800, 50},
        {3900, 70}, {4000, 85}, {4150, 100},
    };
    const uint16_t mv = b.millivolts;
    if (mv == 0) return b;                 // no sensible reading
    if (mv <= kCurve[0].mv) {
        b.percent = 0;
    } else if (mv >= kCurve[6].mv) {
        b.percent = 100;
    } else {
        for (size_t i = 1; i < sizeof(kCurve) / sizeof(kCurve[0]); ++i) {
            if (mv <= kCurve[i].mv) {
                const Point& a = kCurve[i - 1];
                const Point& c = kCurve[i];
                b.percent = static_cast<uint8_t>(
                    a.pct + (mv - a.mv) * (c.pct - a.pct) / (c.mv - a.mv));
                break;
            }
        }
    }
    b.valid = true;
    return b;
}

void dumpRegisters(uint8_t first, uint8_t last) {
    if (!s_available) {
        ESP_LOGE(kTag, "PMIC unavailable");
        return;
    }
    ESP_LOGW(kTag, "PMIC 0x%02X register dump 0x%02X..0x%02X", pins::kAddrPm1,
             first, last);
    char line[80];
    for (uint8_t base = first & 0xF0u; base <= (last | 0x0Fu); base += 16) {
        int n = std::snprintf(line, sizeof(line), "  %02X:", base);
        for (uint8_t i = 0; i < 16; ++i) {
            const uint8_t reg = static_cast<uint8_t>(base + i);
            if (reg < first || reg > last) {
                n += std::snprintf(line + n, sizeof(line) - n, " --");
                continue;
            }
            uint8_t v = 0;
            n += std::snprintf(line + n, sizeof(line) - n,
                               rd(reg, &v) ? " %02X" : " ??", v);
        }
        ESP_LOGW(kTag, "%s", line);
        if (base > 0xF0u - 16u) break;
    }
}

void dumpPowerState() {
    if (!s_available) {
        ESP_LOGE(kTag, "PMIC unavailable");
        return;
    }
    const Battery b = readBattery();
    ESP_LOGW(kTag, "--- power state ---");
    if (b.valid) {
        ESP_LOGW(kTag, "battery      : %u mV  (~%u%%)  %s", b.millivolts,
                 b.percent, b.charging ? "on external 5V" : "on battery");
    } else {
        ESP_LOGW(kTag, "battery      : no valid reading (%u mV raw)",
                 b.millivolts);
    }

    uint8_t src = 0, lvp = 0, i2ccfg = 0, wake = 0, cfg = 0;
    if (rd(kRegPwrSrc, &src)) {
        const char* names[] = {"5VIN (USB/DC)", "5VINOUT", "BATTERY"};
        const uint8_t i = src & 0x07;
        ESP_LOGW(kTag, "power source : %s (0x%02X)", i < 3 ? names[i] : "?", src);
    }
    // Keep the cross-check: VIN must read ~5000 mV on USB. If it does not,
    // the decode is wrong again and nothing below should be believed.
    uint16_t vin = 0, vref = 0;
    if (readMillivolts(kRegVinL, kRegVinH, &vin)) {
        ESP_LOGW(kTag, "VIN          : %u mV  %s", vin,
                 (vin > 4300) ? "(external 5V present)"
                              : (vin < 500 ? "(no external supply)"
                                           : "(?? decode suspect)"));
    }
    if (readMillivolts(kRegVrefL, kRegVrefH, &vref)) {
        ESP_LOGW(kTag, "VREF         : %u mV  (expect ~3300)", vref);
    }
    if (rd(kRegBattLvp, &lvp)) {
        // mV = 2000 + n * 7.81
        const uint32_t thresh = 2000u + (lvp * 781u) / 100u;
        ESP_LOGW(kTag, "batt LVP     : %lu mV (raw 0x%02X)",
                 (unsigned long)thresh, lvp);
        if (b.valid && b.millivolts > 0 && b.millivolts < thresh) {
            ESP_LOGE(kTag, "  ** cell is BELOW the protection threshold: the "
                           "PMIC will not start the");
            ESP_LOGE(kTag, "  ** rails from battery alone. Charge it.");
        } else if (b.valid) {
            ESP_LOGW(kTag, "  (cell is %d mV above the threshold -- LVP is NOT "
                           "limiting wake)",
                     static_cast<int>(b.millivolts) - static_cast<int>(thresh));
        }
    }
    if (rd(kRegI2cCfg, &i2ccfg)) {
        const uint8_t slp = i2ccfg & 0x0F;
        ESP_LOGW(kTag, "I2C sleep    : %s (raw 0x%02X)",
                 slp == 0 ? "disabled (correct)" : "ENABLED -- PMIC may nap",
                 i2ccfg);
    }
    if (rd(kRegWakeSrc, &wake)) {
        ESP_LOGW(kTag, "wake source  : 0x%02X%s%s%s%s", wake,
                 (wake & 0x04) ? " power-btn" : "",
                 (wake & 0x02) ? " vin-insert" : "",
                 (wake & 0x40) ? " 5vinout-insert" : "",
                 (wake & 0x01) ? " timer" : "");
    }
    if (rd(kRegPwrCfg, &cfg)) {
        ESP_LOGW(kTag, "rails        : LDO3V3=%d DCDC5V=%d BOOST=%d CHG=%d "
                       "(raw 0x%02X)",
                 (cfg >> 2) & 1, (cfg >> 1) & 1, (cfg >> 3) & 1, cfg & 1, cfg);
    }
    ESP_LOGW(kTag, "--- end ---");
}

void enterDeepSleep() {
    // Buttons are active-low, so wake on ANY_LOW. All three are inside the
    // ESP32-S3's 22 RTC-capable GPIOs (SOC_RTCIO_PIN_COUNT == 22), which is
    // what makes them usable as EXT1 sources at all.
    const uint64_t mask = (1ULL << pins::kBtnA) | (1ULL << pins::kBtnB) |
                          (1ULL << pins::kBtnC);

    // Hold the pins up through sleep. Without this they can float once the
    // digital domain goes down and wake the board immediately.
    const gpio_num_t wake_pins[] = {pins::kBtnA, pins::kBtnB, pins::kBtnC};
    for (const gpio_num_t pin : wake_pins) {
        rtc_gpio_init(pin);
        rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pulldown_dis(pin);
        rtc_gpio_pullup_en(pin);
    }

    const esp_err_t err =
        esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "ext1 wake config failed: %s", esp_err_to_name(err));
    }

    ESP_LOGW(kTag, "entering deep sleep; wake on GPIO%d/%d/%d (any button)",
             pins::kBtnA, pins::kBtnB, pins::kBtnC);
    ESP_LOGW(kTag, "the e-paper keeps its image at zero power");
    // Let the log drain before the UART dies with the rest of the chip.
    vTaskDelay(pdMS_TO_TICKS(120));

    esp_deep_sleep_start();   // never returns; wake is a full reset
}

void powerOff() {
    if (!s_available) {
        ESP_LOGE(kTag, "PMIC unavailable; cannot power off");
        return;
    }
    ESP_LOGW(kTag, "cutting power via PMIC; quick-press PWR_KEY to wake "
                   "(do NOT hold -- holding enters download mode)");
    // The PMIC wants a settle window before it will accept the command.
    vTaskDelay(pdMS_TO_TICKS(120));
    if (!wr(kRegSysCmd, static_cast<uint8_t>(kSysCmdKey | kSysCmdShutdown))) {
        ESP_LOGE(kTag, "shutdown command write failed");
        return;
    }
    // Rails drop within a few ms. If we are still executing after this, the
    // PMIC did not reset the chip -- which is the documented behaviour on
    // battery power -- and continuing would leave the device half-powered:
    // LEDs alive, panel and SD dead. Deep sleep instead, which is a state we
    // can actually get out of.
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGW(kTag, "PMIC shutdown did not reset the chip (expected on battery);"
                   " falling back to deep sleep");
    enterDeepSleep();
}

void ledSet(Led state) {
    // The rainbow owns the LEDs while it runs; status colours would just be
    // overwritten 20 times a second.
    if (s_rainbow_run) return;
    if (!s_led_ok) return;
    uint8_t r = 0, g = 0, b = 0;
    switch (state) {
        case Led::kIdle:     r = 0;   g = 0;   b = 0;   break;
        case Led::kBusy:     r = 20;  g = 20;  b = 200; break;
        case Led::kSpeaking: r = 20;  g = 180; b = 40;  break;
        case Led::kError:    r = 200; g = 0;   b = 0;   break;
    }
    M5.Led.setAllColor(r, g, b);
    M5.Led.display();
}

void ledBrightness(uint8_t brightness) {
    if (!s_led_ok) return;
    M5.Led.setBrightness(brightness);
}

}  // namespace hal::power
