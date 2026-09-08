#include "hal/hal_input.h"
#include "hal/hal_pins.h"

#include <cstdio>

#include <esp_log.h>
#include <M5Unified.hpp>

namespace hal::input {
namespace {

constexpr const char* kTag = "input";

// ---------------------------------------------------------------------------
// LOGICAL -> PHYSICAL MAPPING.  Edit these three lines if `btn` shows the
// labels are back-to-front on real hardware.
//   index 0 = kTop, 1 = kMiddle, 2 = kBottom
//   value  = 0 -> BtnA (GPIO9), 1 -> BtnB (GPIO10), 2 -> BtnC (GPIO1)
// ---------------------------------------------------------------------------
// index 0 = kNextCard, 1 = kRepeat, 2 = kLetterInWord
// value 0 -> BtnA (GPIO9), 1 -> BtnB (GPIO10), 2 -> BtnC (GPIO1)
//
// kNextCard is the UPPER side button = GPIO10 = BtnB, and kRepeat is the
// LOWER one = GPIO9 = BtnA -- hence the swap of the first two entries.
constexpr uint8_t kLogicalToPhysical[3] = {1, 0, 2};

constexpr const char* kLabels[3] = {
    "upper side (BtnB/GPIO10) -> next card",
    "lower side (BtnA/GPIO9) -> repeat",
    "top (BtnC/GPIO1) -> next letter in word",
};

char s_snapshot[64];

m5::Button_Class* physical(uint8_t idx) {
    switch (idx) {
        case 0: return &M5.BtnA;
        case 1: return &M5.BtnB;
        case 2: return &M5.BtnC;
        default: return nullptr;
    }
}

m5::Button_Class* forLogical(Button b) {
    const auto i = static_cast<uint8_t>(b);
    if (i >= 3) return nullptr;
    return physical(kLogicalToPhysical[i]);
}

}  // namespace

void init() {
    ESP_LOGI(kTag, "buttons: GPIO%d, GPIO%d, GPIO%d (active-low). The 4th "
                   "(lowest side) is the PMIC power button and is not a GPIO.",
             pins::kBtnA, pins::kBtnB, pins::kBtnC);
    ESP_LOGI(kTag, "GPIO10 upper side = next card | GPIO9 lower side = repeat "
                   "| GPIO1 top = next letter in word | PWR_KEY = power");
}

void update() { M5.update(); }

bool wasPressed(Button b) {
    auto* btn = forLogical(b);
    return btn != nullptr && btn->wasClicked();
}

bool isHeld(Button b) {
    auto* btn = forLogical(b);
    return btn != nullptr && btn->isPressed();
}

const char* rawSnapshot() {
    std::snprintf(s_snapshot, sizeof(s_snapshot), "A(gpio9)=%s B(gpio10)=%s C(gpio1)=%s",
                  M5.BtnA.isPressed() ? "DOWN" : "up",
                  M5.BtnB.isPressed() ? "DOWN" : "up",
                  M5.BtnC.isPressed() ? "DOWN" : "up");
    return s_snapshot;
}

const char* label(Button b) {
    const auto i = static_cast<uint8_t>(b);
    return i < 3 ? kLabels[i] : "?";
}

}  // namespace hal::input
