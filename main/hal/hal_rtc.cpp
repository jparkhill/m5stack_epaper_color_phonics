#include "hal/hal_rtc.h"
#include "hal/hal_pins.h"

#include <cstdio>
#include <cstring>
#include <sys/time.h>

#include <esp_log.h>
#include <M5Unified.hpp>

namespace hal::rtc {
namespace {

constexpr const char* kTag = "rtc";
constexpr uint32_t kI2cFreq = 100000;

// RX8130 register map (only these are used).
constexpr uint8_t kRegSecond = 0x10;  // 0x10..0x16 = sec,min,hour,week,day,month,year
constexpr uint8_t kRegFlag   = 0x1E;
constexpr uint8_t kFlagVlf   = 0x02;  // bit1: voltage-low -> timekeeping lost

bool s_available = false;
bool s_suspect = false;

uint8_t bcdToDec(uint8_t v) { return static_cast<uint8_t>((v >> 4) * 10 + (v & 0x0F)); }
uint8_t decToBcd(uint8_t v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }

/// The WEEK register is a one-hot bitmask, not a number.
uint8_t weekdayFromMask(uint8_t mask) {
    for (uint8_t i = 0; i < 7; ++i) {
        if (mask & (1 << i)) return i;
    }
    return 0;
}

bool plausible(const DateTime& dt) {
    return dt.year >= 2020 && dt.year <= 2099 &&
           dt.month >= 1 && dt.month <= 12 &&
           dt.day >= 1 && dt.day <= 31 &&
           dt.hour <= 23 && dt.minute <= 59 && dt.second <= 59;
}

}  // namespace

DateTime buildTimestamp() {
    // __DATE__ is "Mmm dd yyyy", __TIME__ is "hh:mm:ss".
    static const char* kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int day = 1, year = 2026, hh = 0, mm = 0, ss = 0;
    std::sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
    std::sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);

    int month = 1;
    const char* p = std::strstr(kMonths, mon);
    if (p != nullptr) month = static_cast<int>((p - kMonths) / 3) + 1;

    DateTime dt{};
    dt.year = static_cast<uint16_t>(year);
    dt.month = static_cast<uint8_t>(month);
    dt.day = static_cast<uint8_t>(day);
    dt.hour = static_cast<uint8_t>(hh);
    dt.minute = static_cast<uint8_t>(mm);
    dt.second = static_cast<uint8_t>(ss);

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hh;
    tm.tm_min = mm;
    tm.tm_sec = ss;
    const std::time_t t = std::mktime(&tm);
    const std::tm* norm = std::localtime(&t);
    dt.weekday = norm ? static_cast<uint8_t>(norm->tm_wday) : 0;
    return dt;
}

esp_err_t init() {
    // Pin the timezone to UTC0 so the RTC's local wall-clock time passes
    // through to strftime() untouched.
    setenv("TZ", "UTC0", 1);
    tzset();

    if (!M5.In_I2C.scanID(pins::kAddrRx8130, kI2cFreq)) {
        ESP_LOGE(kTag, "RX8130 not responding at 0x%02X", pins::kAddrRx8130);
        s_available = false;
        return ESP_ERR_NOT_FOUND;
    }
    s_available = true;

    uint8_t flag = 0;
    if (M5.In_I2C.readRegister(pins::kAddrRx8130, kRegFlag, &flag, 1, kI2cFreq)) {
        s_suspect = (flag & kFlagVlf) != 0;
    } else {
        ESP_LOGW(kTag, "could not read flag register; assuming time is suspect");
        s_suspect = true;
    }

    DateTime dt{};
    const bool got = get(&dt);

    if (s_suspect || !got || !plausible(dt)) {
        const DateTime seed = buildTimestamp();
        ESP_LOGW(kTag, "**********************************************************");
        ESP_LOGW(kTag, "RTC time is NOT trustworthy (VLF=%d, read=%d).",
                 static_cast<int>(s_suspect), static_cast<int>(got));
        ESP_LOGW(kTag, "Seeding from the firmware build stamp: %04u-%02u-%02u %02u:%02u:%02u",
                 seed.year, seed.month, seed.day, seed.hour, seed.minute, seed.second);
        ESP_LOGW(kTag, "Run:  time %04u-%02u-%02u HH:MM:SS   to set the real time,",
                 seed.year, seed.month, seed.day);
        ESP_LOGW(kTag, "or just run tools/set_time.py from the host.");
        ESP_LOGW(kTag, "**********************************************************");
        set(seed);
        s_suspect = true;   // still a guess until someone sets it for real
        dt = seed;
    } else {
        ESP_LOGI(kTag, "RX8130 ok: %04u-%02u-%02u %02u:%02u:%02u (weekday %u)",
                 dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, dt.weekday);
    }

    syncSystemClock();
    return ESP_OK;
}

bool available() { return s_available; }
bool timeIsSuspect() { return s_suspect; }

bool get(DateTime* out) {
    if (!s_available || out == nullptr) return false;
    uint8_t buf[7] = {0};
    if (!M5.In_I2C.readRegister(pins::kAddrRx8130, kRegSecond, buf, sizeof(buf), kI2cFreq)) {
        return false;
    }
    out->second  = bcdToDec(buf[0] & 0x7F);
    out->minute  = bcdToDec(buf[1] & 0x7F);
    out->hour    = bcdToDec(buf[2] & 0x3F);
    out->weekday = weekdayFromMask(buf[3] & 0x7F);
    out->day     = bcdToDec(buf[4] & 0x3F);
    out->month   = bcdToDec(buf[5] & 0x1F);
    out->year    = static_cast<uint16_t>(2000 + bcdToDec(buf[6]));
    return true;
}

bool set(const DateTime& dt) {
    if (!s_available) return false;
    if (!plausible(dt)) {
        ESP_LOGE(kTag, "refusing to write implausible time %04u-%02u-%02u %02u:%02u:%02u",
                 dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
        return false;
    }

    // Recompute the weekday rather than trusting the caller.
    std::tm tm{};
    tm.tm_year = dt.year - 1900;
    tm.tm_mon = dt.month - 1;
    tm.tm_mday = dt.day;
    tm.tm_hour = dt.hour;
    tm.tm_min = dt.minute;
    tm.tm_sec = dt.second;
    const std::time_t t = std::mktime(&tm);
    const std::tm* norm = std::localtime(&t);
    const uint8_t wday = norm ? static_cast<uint8_t>(norm->tm_wday) : 0;

    uint8_t buf[7];
    buf[0] = decToBcd(dt.second);
    buf[1] = decToBcd(dt.minute);
    buf[2] = decToBcd(dt.hour);
    buf[3] = static_cast<uint8_t>(1u << wday);          // one-hot weekday
    buf[4] = decToBcd(dt.day);
    buf[5] = decToBcd(dt.month);
    buf[6] = decToBcd(static_cast<uint8_t>(dt.year % 100));

    if (!M5.In_I2C.writeRegister(pins::kAddrRx8130, kRegSecond, buf, sizeof(buf), kI2cFreq)) {
        ESP_LOGE(kTag, "failed writing time registers");
        return false;
    }

    // Clear the voltage-low flag so a later boot trusts this value.
    uint8_t flag = 0;
    if (M5.In_I2C.readRegister(pins::kAddrRx8130, kRegFlag, &flag, 1, kI2cFreq)) {
        flag = static_cast<uint8_t>(flag & ~kFlagVlf);
        M5.In_I2C.writeRegister(pins::kAddrRx8130, kRegFlag, &flag, 1, kI2cFreq);
    }

    syncSystemClock();
    return true;
}

bool setFromString(const char* iso) {
    if (iso == nullptr) return false;
    DateTime dt{};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    const int n = std::sscanf(iso, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s);
    if (n < 5) {
        ESP_LOGE(kTag, "expected \"YYYY-MM-DD HH:MM[:SS]\", got \"%s\"", iso);
        return false;
    }
    dt.year = static_cast<uint16_t>(y);
    dt.month = static_cast<uint8_t>(mo);
    dt.day = static_cast<uint8_t>(d);
    dt.hour = static_cast<uint8_t>(h);
    dt.minute = static_cast<uint8_t>(mi);
    dt.second = static_cast<uint8_t>(n >= 6 ? s : 0);
    if (!set(dt)) return false;
    s_suspect = false;   // explicitly set by a human/host: trust it
    ESP_LOGI(kTag, "time set to %04u-%02u-%02u %02u:%02u:%02u",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    return true;
}

bool syncSystemClock() {
    DateTime dt{};
    if (!get(&dt) || !plausible(dt)) return false;

    std::tm tm{};
    tm.tm_year = dt.year - 1900;
    tm.tm_mon = dt.month - 1;
    tm.tm_mday = dt.day;
    tm.tm_hour = dt.hour;
    tm.tm_min = dt.minute;
    tm.tm_sec = dt.second;
    tm.tm_isdst = 0;

    const std::time_t t = std::mktime(&tm);
    if (t == static_cast<std::time_t>(-1)) return false;

    timeval tv{};
    tv.tv_sec = t;
    tv.tv_usec = 0;
    return settimeofday(&tv, nullptr) == 0;
}

}  // namespace hal::rtc
