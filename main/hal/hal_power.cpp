#include "hal/hal_power.h"
#include "hal/hal_pins.h"

#include <cmath>

#include <esp_log.h>
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
    // Rails drop within a few ms; if we are still here after this the command
    // did not take.
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGE(kTag, "still running after shutdown command -- is the board on "
                   "external USB power that overrides the PMIC?");
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
