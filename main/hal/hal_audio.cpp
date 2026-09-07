#include "hal/hal_audio.h"
#include "hal/hal_pins.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <M5Unified.hpp>

namespace hal::audio {
namespace {

constexpr const char* kTag = "audio";
constexpr uint8_t kDefaultVolume = 200;

// ES8311 codec on the internal I2C bus. This is the 0x18 device that shows up
// in the boot I2C scan.
constexpr uint8_t kCodecAddr = 0x18;
constexpr uint32_t kCodecFreq = 100000;

// Codec power / amplifier enables.
constexpr gpio_num_t kCodecEnPin = GPIO_NUM_45;
constexpr gpio_num_t kSpkEnPin   = GPIO_NUM_46;

// Register sequence lifted from M5Unified's _speaker_enabled_cb_papercolor.
struct CodecReg { uint8_t reg; uint8_t val; };
constexpr CodecReg kCodecInit[] = {
    {0x00, 0x80},  // RESET: CSM power on
    {0x01, 0xB5},  // CLOCK_MANAGER: derive the codec clock from BCLK
    {0x02, 0x18},  // CLOCK_MANAGER: MULT_PRE = 3
    {0x0D, 0x01},  // SYSTEM: power up analog
    {0x12, 0x00},  // SYSTEM: power up DAC
    {0x13, 0x10},  // SYSTEM: enable output to the headphone driver
    {0x32, 0xCF},  // DAC volume (+16 dB)
    {0x37, 0x08},  // DAC: bypass equaliser
};

// One reusable DMA staging buffer. Mono input is expanded to stereo here
// because the codec is wired for two slots.
constexpr size_t kChunkFrames = 512;

i2s_chan_handle_t s_tx = nullptr;
bool s_available = false;
bool s_enabled = false;
uint32_t s_rate = 0;
uint8_t s_volume = kDefaultVolume;
int16_t* s_chunk = nullptr;          // kChunkFrames * 2 samples

// PCM scratch for file playback (PSRAM).
int16_t* s_pcm = nullptr;
size_t s_pcm_cap = 0;

struct WavInfo {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
    uint32_t data_offset;
    uint32_t data_bytes;
};

uint32_t rd32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t rd16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

bool writeCodec(uint8_t reg, uint8_t val) {
    return M5.In_I2C.writeRegister(kCodecAddr, reg, &val, 1, kCodecFreq);
}

esp_err_t initCodec() {
    gpio_config_t io{};
    io.pin_bit_mask = (1ULL << kCodecEnPin) | (1ULL << kSpkEnPin);
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io));

    gpio_set_level(kCodecEnPin, 1);
    gpio_set_level(kSpkEnPin, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    if (!M5.In_I2C.scanID(kCodecAddr, kCodecFreq)) {
        ESP_LOGE(kTag, "ES8311 codec not responding at 0x%02X", kCodecAddr);
        return ESP_ERR_NOT_FOUND;
    }

    for (const auto& r : kCodecInit) {
        if (!writeCodec(r.reg, r.val)) {
            ESP_LOGE(kTag, "codec write 0x%02X=0x%02X failed", r.reg, r.val);
            return ESP_FAIL;
        }
        // The reset register in particular needs a moment to settle.
        vTaskDelay(pdMS_TO_TICKS(r.reg == 0x00 ? 20 : 2));
    }
    ESP_LOGI(kTag, "ES8311 configured (%u registers)",
             (unsigned)(sizeof(kCodecInit) / sizeof(kCodecInit[0])));
    return ESP_OK;
}

i2s_std_clk_config_t clockFor(uint32_t rate) {
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    return clk;
}

esp_err_t setRate(uint32_t rate) {
    if (rate == s_rate) return ESP_OK;
    if (s_enabled) {
        i2s_channel_disable(s_tx);
        s_enabled = false;
    }
    i2s_std_clk_config_t clk = clockFor(rate);
    const esp_err_t err = i2s_channel_reconfig_std_clock(s_tx, &clk);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "reconfig to %luHz failed: %s", (unsigned long)rate,
                 esp_err_to_name(err));
        return err;
    }
    s_rate = rate;
    ESP_LOGD(kTag, "I2S clock set to %luHz", (unsigned long)rate);
    return ESP_OK;
}

bool enable() {
    if (s_enabled) return true;
    const esp_err_t err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_channel_enable: %s", esp_err_to_name(err));
        return false;
    }
    s_enabled = true;
    return true;
}

void disable() {
    if (!s_enabled) return;
    i2s_channel_disable(s_tx);
    s_enabled = false;
}

/// Push interleaved-stereo PCM out of the I2S port, applying software volume
/// and expanding mono to stereo on the way.
bool pushPcm(const int16_t* pcm, size_t frames, uint16_t channels) {
    if (!s_available || pcm == nullptr || frames == 0) return false;
    if (!enable()) return false;

    const int32_t gain = static_cast<int32_t>(s_volume);
    size_t done = 0;
    while (done < frames) {
        const size_t n = (frames - done) < kChunkFrames ? (frames - done) : kChunkFrames;
        for (size_t i = 0; i < n; ++i) {
            int32_t l, r;
            if (channels == 2) {
                l = pcm[(done + i) * 2];
                r = pcm[(done + i) * 2 + 1];
            } else {
                l = r = pcm[done + i];
            }
            // volume 0..255 maps to 0..1.0
            l = (l * gain) >> 8;
            r = (r * gain) >> 8;
            s_chunk[i * 2] = static_cast<int16_t>(l);
            s_chunk[i * 2 + 1] = static_cast<int16_t>(r);
        }
        size_t written = 0;
        const esp_err_t err = i2s_channel_write(s_tx, s_chunk, n * 2 * sizeof(int16_t),
                                                &written, 2000);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "i2s write failed: %s", esp_err_to_name(err));
            return false;
        }
        done += n;
    }

    // Flush a little silence so the codec does not hold the last sample and
    // click when the channel stops.
    std::memset(s_chunk, 0, kChunkFrames * 2 * sizeof(int16_t));
    size_t written = 0;
    i2s_channel_write(s_tx, s_chunk, kChunkFrames * 2 * sizeof(int16_t), &written, 500);
    return true;
}

bool parseWavMemory(const unsigned char* d, unsigned int n, WavInfo* out) {
    if (n < 44 || std::memcmp(d, "RIFF", 4) != 0 ||
        std::memcmp(d + 8, "WAVE", 4) != 0) {
        ESP_LOGE(kTag, "not a RIFF/WAVE blob");
        return false;
    }
    bool have_fmt = false, have_data = false;
    unsigned int pos = 12;
    // Walk the chunk list rather than assuming a 44-byte header: encoders
    // routinely insert LIST/fact chunks before `data`.
    while (pos + 8 <= n && !(have_fmt && have_data)) {
        const unsigned char* ch = d + pos;
        const uint32_t size = rd32(ch + 4);
        if (std::memcmp(ch, "fmt ", 4) == 0 && pos + 24 <= n) {
            if (rd16(ch + 8) != 1) {
                ESP_LOGE(kTag, "WAV is not PCM");
                return false;
            }
            out->channels = rd16(ch + 10);
            out->sample_rate = rd32(ch + 12);
            out->bits = rd16(ch + 22);
            have_fmt = true;
        } else if (std::memcmp(ch, "data", 4) == 0) {
            out->data_offset = pos + 8;
            out->data_bytes = size;
            have_data = true;
        }
        pos += 8 + size + (size & 1u);
    }
    if (!have_fmt || !have_data) {
        ESP_LOGE(kTag, "WAV missing %s chunk", !have_fmt ? "fmt " : "data");
        return false;
    }
    if (out->bits != 16 || (out->channels != 1 && out->channels != 2)) {
        ESP_LOGE(kTag, "need 16-bit mono/stereo, got %u-bit %uch", out->bits,
                 out->channels);
        return false;
    }
    if (out->data_offset + out->data_bytes > n) {
        out->data_bytes = n - out->data_offset;   // truncated file
    }
    return true;
}

bool ensurePcm(size_t bytes) {
    if (bytes <= s_pcm_cap) return true;
    void* p = heap_caps_realloc(s_pcm, bytes, MALLOC_CAP_SPIRAM);
    if (p == nullptr) {
        ESP_LOGE(kTag, "PSRAM alloc of %u bytes failed", (unsigned)bytes);
        return false;
    }
    s_pcm = static_cast<int16_t*>(p);
    s_pcm_cap = bytes;
    return true;
}

}  // namespace

esp_err_t init() {
    s_available = false;

    esp_err_t err = initCodec();
    if (err != ESP_OK) return err;

    s_chunk = static_cast<int16_t*>(heap_caps_calloc(
        kChunkFrames * 2, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (s_chunk == nullptr) {
        ESP_LOGE(kTag, "could not allocate the DMA staging buffer");
        return ESP_ERR_NO_MEM;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear = true;
    err = i2s_new_channel(&chan_cfg, &s_tx, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg{};
    std_cfg.clk_cfg = clockFor(22050);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                           I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = pins::kSpkMclk;
    std_cfg.gpio_cfg.bclk = pins::kSpkBclk;
    std_cfg.gpio_cfg.ws   = pins::kSpkWs;
    std_cfg.gpio_cfg.dout = pins::kSpkData;
    std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv   = false;

    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_channel_init_std_mode: %s", esp_err_to_name(err));
        return err;
    }
    s_rate = 22050;
    s_volume = kDefaultVolume;
    s_available = true;

    ESP_LOGI(kTag, "I2S TX up (MCK=%d BCK=%d WS=%d DOUT=%d), codec 0x%02X, vol %u",
             pins::kSpkMclk, pins::kSpkBclk, pins::kSpkWs, pins::kSpkData,
             kCodecAddr, s_volume);
    return ESP_OK;
}

bool available() { return s_available; }

bool playWavMemory(const unsigned char* data, unsigned int len, const char* label) {
    if (!s_available || data == nullptr) return false;
    WavInfo info{};
    if (!parseWavMemory(data, len, &info)) return false;

    const int16_t* pcm = reinterpret_cast<const int16_t*>(data + info.data_offset);
    const size_t frames = info.data_bytes / (sizeof(int16_t) * info.channels);
    ESP_LOGI(kTag, "play %s from flash (%.2fs, %luHz, %s)", label ? label : "?",
             static_cast<float>(frames) / static_cast<float>(info.sample_rate),
             (unsigned long)info.sample_rate, info.channels == 2 ? "stereo" : "mono");

    if (setRate(info.sample_rate) != ESP_OK) return false;
    const bool ok = pushPcm(pcm, frames, info.channels);
    disable();
    return ok;
}

bool playWavFile(const char* path) {
    if (!s_available || path == nullptr) return false;

    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGE(kTag, "cannot open %s", path);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 44) {
        ESP_LOGE(kTag, "%s is too small to be a WAV (%ld bytes)", path, size);
        std::fclose(f);
        return false;
    }
    if (!ensurePcm(static_cast<size_t>(size))) {
        std::fclose(f);
        return false;
    }
    // Read the whole clip before touching the panel-shared SPI bus again.
    const size_t got = std::fread(s_pcm, 1, static_cast<size_t>(size), f);
    std::fclose(f);
    if (got == 0) {
        ESP_LOGE(kTag, "read 0 bytes from %s", path);
        return false;
    }

    WavInfo info{};
    const auto* bytes = reinterpret_cast<const unsigned char*>(s_pcm);
    if (!parseWavMemory(bytes, static_cast<unsigned int>(got), &info)) return false;

    const int16_t* pcm = reinterpret_cast<const int16_t*>(bytes + info.data_offset);
    const size_t frames = info.data_bytes / (sizeof(int16_t) * info.channels);
    ESP_LOGI(kTag, "play %s (%.2fs, %luHz, %s)", path,
             static_cast<float>(frames) / static_cast<float>(info.sample_rate),
             (unsigned long)info.sample_rate, info.channels == 2 ? "stereo" : "mono");

    if (setRate(info.sample_rate) != ESP_OK) return false;
    const bool ok = pushPcm(pcm, frames, info.channels);
    disable();
    return ok;
}

void tone(float freq_hz, uint32_t ms) {
    if (!s_available || ms == 0) return;
    constexpr uint32_t kToneRate = 22050;
    if (setRate(kToneRate) != ESP_OK) return;

    const size_t frames = (kToneRate * ms) / 1000;
    if (!enable()) return;

    size_t done = 0;
    const float step = 2.0f * static_cast<float>(M_PI) * freq_hz /
                       static_cast<float>(kToneRate);
    const int32_t gain = static_cast<int32_t>(s_volume);
    while (done < frames) {
        const size_t n = (frames - done) < kChunkFrames ? (frames - done) : kChunkFrames;
        for (size_t i = 0; i < n; ++i) {
            // Fade the last few ms so the tone does not end in a click.
            const size_t idx = done + i;
            float amp = 0.28f;
            const size_t fade = kToneRate / 100;   // 10ms
            if (frames > fade && idx > frames - fade) {
                amp *= static_cast<float>(frames - idx) / static_cast<float>(fade);
            }
            const float v = std::sin(step * static_cast<float>(idx)) * amp * 32767.0f;
            int32_t s = static_cast<int32_t>(v);
            s = (s * gain) >> 8;
            s_chunk[i * 2] = static_cast<int16_t>(s);
            s_chunk[i * 2 + 1] = static_cast<int16_t>(s);
        }
        size_t written = 0;
        if (i2s_channel_write(s_tx, s_chunk, n * 2 * sizeof(int16_t), &written,
                              1000) != ESP_OK) {
            break;
        }
        done += n;
    }
    disable();
}

void chirp() { tone(1200.0f, 70); }

void bootChime() {
    tone(880.0f, 110);
    tone(1320.0f, 130);
}

bool isPlaying() { return false; }   // synchronous playback

void stop() { disable(); }

void setVolume(uint8_t v) { s_volume = v; }
uint8_t volume() { return s_volume; }

}  // namespace hal::audio
