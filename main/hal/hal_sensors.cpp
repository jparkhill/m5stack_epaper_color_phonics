#include "hal/hal_sensors.h"
#include "hal/hal_pins.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <M5Unified.hpp>

namespace hal::sensors {
namespace {

constexpr const char* kTag = "sensors";
constexpr uint8_t kCmdHighPrecision = 0xFD;
constexpr uint32_t kI2cFreq = 400000;

Reading s_last{};

/// SHT4x CRC-8: polynomial 0x31, init 0xFF. Catches a floating SDA line
/// returning plausible-looking garbage.
uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                               : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

}  // namespace

esp_err_t init() {
    if (!M5.In_I2C.scanID(pins::kAddrSht40, 100000)) {
        ESP_LOGE(kTag, "SHT40 not responding at 0x%02X", pins::kAddrSht40);
        return ESP_ERR_NOT_FOUND;
    }
    Reading r{};
    if (!read(&r)) {
        ESP_LOGE(kTag, "SHT40 present but first measurement failed");
        return ESP_FAIL;
    }
    ESP_LOGI(kTag, "SHT40 ok: %.1f C, %.0f%% RH", r.temperature_c, r.humidity_pct);
    return ESP_OK;
}

bool read(Reading* out) {
    if (out == nullptr) return false;
    out->valid = false;

    // Up to 4 attempts. The SHT4x NACKs reads until its conversion finishes,
    // and I2C_Class::read() reports failure in that case, so a single
    // marginally-early read looks like a dead sensor.
    for (int attempt = 0; attempt < 4; ++attempt) {
        uint8_t cmd = kCmdHighPrecision;
        if (!M5.In_I2C.start(pins::kAddrSht40, false, kI2cFreq)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        M5.In_I2C.write(&cmd, 1);
        M5.In_I2C.stop();

        // Datasheet: high-precision conversion takes up to 8.3ms.
        vTaskDelay(pdMS_TO_TICKS(12));

        uint8_t buf[6] = {0};
        if (!M5.In_I2C.start(pins::kAddrSht40, true, kI2cFreq)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        // Deliberately ignoring the return value and letting the CRC decide:
        // this driver's read() returns false in cases where the bytes are in
        // fact good, and a passing CRC-8 is far stronger evidence than the
        // transfer's own status.
        M5.In_I2C.read(buf, sizeof(buf));
        M5.In_I2C.stop();

        if (crc8(&buf[0], 2) != buf[2] || crc8(&buf[3], 2) != buf[5]) {
            ESP_LOGD(kTag, "attempt %d: CRC mismatch (%02x%02x/%02x %02x%02x/%02x)",
                     attempt, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
            vTaskDelay(pdMS_TO_TICKS(8));
            continue;
        }

        const uint16_t raw_t = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
        const uint16_t raw_h = static_cast<uint16_t>((buf[3] << 8) | buf[4]);

        out->temperature_c = -45.0f + 175.0f * (static_cast<float>(raw_t) / 65535.0f);
        float rh = -6.0f + 125.0f * (static_cast<float>(raw_h) / 65535.0f);
        if (rh < 0.0f) rh = 0.0f;
        if (rh > 100.0f) rh = 100.0f;
        out->humidity_pct = rh;
        out->valid = true;
        if (attempt > 0) {
            ESP_LOGD(kTag, "read succeeded on attempt %d", attempt + 1);
        }
        return true;
    }

    ESP_LOGW(kTag, "SHT40 gave no CRC-valid sample in 4 attempts");
    return false;
}

const Reading& last() { return s_last; }

bool refresh() {
    Reading r{};
    if (!read(&r)) return false;
    s_last = r;
    return true;
}

}  // namespace hal::sensors
