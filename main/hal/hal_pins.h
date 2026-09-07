/*
 * Central pin map for M5Stack PaperColor (SKU C151, ESP32-S3R8).
 *
 * Every value here was read out of M5GFX's board autodetect block for
 * board_M5PaperColor and M5Unified's pin tables, so it matches what those
 * libraries actually program. Keep this file as the single source of truth.
 */
#pragma once

#include <driver/gpio.h>

namespace hal::pins {

// ---------------------------------------------------------------------------
// Shared SPI2 bus. The e-paper panel and the microSD card sit on the SAME bus
// (M5GFX sets bus_shared = true). Never drive both at once: read card assets
// into PSRAM first, then refresh the panel.
// ---------------------------------------------------------------------------
constexpr gpio_num_t kSpiMosi = GPIO_NUM_13;
constexpr gpio_num_t kSpiMiso = GPIO_NUM_14;
constexpr gpio_num_t kSpiSclk = GPIO_NUM_15;

// e-paper (Panel_ED2208, 400x600 Spectra 6)
constexpr gpio_num_t kEpdCs   = GPIO_NUM_44;
constexpr gpio_num_t kEpdDc   = GPIO_NUM_43;
constexpr gpio_num_t kEpdBusy = GPIO_NUM_11;
constexpr gpio_num_t kEpdRst  = GPIO_NUM_12;

// microSD (SPI mode -- not the 4-bit SDMMC bus)
constexpr gpio_num_t kSdCs = GPIO_NUM_47;

// ---------------------------------------------------------------------------
// Internal I2C: RX8130 RTC @0x32, SHT40 temp/humidity @0x44, M5PM1 PMIC.
// ---------------------------------------------------------------------------
constexpr gpio_num_t kI2cSda = GPIO_NUM_3;
constexpr gpio_num_t kI2cScl = GPIO_NUM_2;

constexpr uint8_t kAddrRx8130 = 0x32;
constexpr uint8_t kAddrSht40  = 0x44;
constexpr uint8_t kAddrPm1    = 0x6E;  // M5PM1_DEFAULT_ADDR

// ---------------------------------------------------------------------------
// I2S speaker. Driven through M5.Speaker; listed for reference/debug only.
// ---------------------------------------------------------------------------
constexpr gpio_num_t kSpkMclk = GPIO_NUM_42;
constexpr gpio_num_t kSpkBclk = GPIO_NUM_40;
constexpr gpio_num_t kSpkWs   = GPIO_NUM_41;
constexpr gpio_num_t kSpkData = GPIO_NUM_38;
constexpr uint32_t kSpkSampleRate = 44100;

// ---------------------------------------------------------------------------
// Buttons. All three are active-low and read directly as GPIOs; M5Unified maps
// them to BtnA/BtnB/BtnC in exactly this order (see its btn_rawstate_bits for
// board_M5PaperColor).
//
// NOTE: which physical button is the "top" one is NOT documented anywhere in
// the M5 sources, so the logical mapping lives in hal_input.h and the `btn`
// console command prints raw state so you can confirm it on the bench in
// seconds rather than guessing.
// ---------------------------------------------------------------------------
constexpr gpio_num_t kBtnA = GPIO_NUM_9;
constexpr gpio_num_t kBtnB = GPIO_NUM_10;
constexpr gpio_num_t kBtnC = GPIO_NUM_1;

// RGB status LED chain (2 LEDs) on the RMT-driven data line.
constexpr gpio_num_t kRgbLed = GPIO_NUM_21;

// ---------------------------------------------------------------------------
// M5PM1 PMIC GPIO assignments (these are PMIC pins, not ESP32 pins).
// ---------------------------------------------------------------------------
constexpr uint8_t kPm1EpdEnable   = 0;  // e-paper rail (M5GFX drives this high)
constexpr uint8_t kPm1SdDetect    = 1;  // card-detect input, active LOW
constexpr uint8_t kPm1RtcWake     = 2;
constexpr uint8_t kPm1SdPowerEn   = 3;  // microSD rail (M5GFX drives this high)
constexpr uint8_t kPm1SdDetectEn  = 4;  // must be HIGH to arm card detect

}  // namespace hal::pins
