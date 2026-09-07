/*
 * SHT40 temperature + relative humidity (internal I2C, 0x44).
 */
#pragma once

#include <cstdint>
#include <esp_err.h>

namespace hal::sensors {

struct Reading {
    float temperature_c{};
    float humidity_pct{};
    bool valid{};
};

esp_err_t init();

/// Blocking single-shot high-precision measurement (~10ms).
bool read(Reading* out);

/// Most recent successful reading. `valid` is false until one succeeds.
const Reading& last();

/// Re-measure and cache. Safe to call often; the sensor is cheap.
bool refresh();

}  // namespace hal::sensors
