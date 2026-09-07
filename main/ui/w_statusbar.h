/*
 * Upper status bar: date, time, temperature, humidity.
 *
 * Occupies theme::kStatus* and nothing else. If the RTC has not been set for
 * real (only seeded from the firmware build stamp) the time is drawn in red
 * with a trailing '?', so a wrong clock is visible on the device rather than
 * only in the serial log.
 */
#pragma once

#include "ui/ui_types.h"

namespace ui {

class StatusBar : public Widget {
public:
    const char* name() const override { return "statusbar"; }
    void draw(M5GFX& g) override;
};

}  // namespace ui
