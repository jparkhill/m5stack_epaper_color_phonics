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
#include <freertos/queue.h>
#include <freertos/semphr.h>
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

// The codec runs at ONE fixed rate and clips are resampled to it.
//
// Reg 0x01 = 0xB5 tells the ES8311 to derive its internal clock from BCLK,
// and reg 0x02 = 0x18 (MULT_PRE = 3) is tuned for that. Reconfiguring the I2S
// clock per clip therefore moves the codec's PLL, and at 22.05kHz it stops
// producing audible output entirely -- the I2S writes still succeed, so it
// looks fine from the firmware side. M5Unified drives this board at a fixed
// 44100 stereo, so we do the same and resample instead.
constexpr uint32_t kCodecRate = 44100;

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
size_t s_pcm_len = 0;          // bytes of the currently preloaded clip

// --- async playback plumbing ---
struct PlayReq {
    const unsigned char* data;   // points into flash, or at s_pcm
    unsigned int len;
    char label[40];
};
QueueHandle_t s_play_q = nullptr;
TaskHandle_t s_play_task = nullptr;
volatile bool s_playing = false;

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

/// Not used in the normal flow -- the channel is brought up at init and left
/// up for the device's lifetime, because start/stop per clip produced an
/// audible transient. Kept for completeness and for stop().
[[maybe_unused]] void disable() {
    if (!s_enabled) return;
    i2s_channel_disable(s_tx);
    s_enabled = false;
}

/// Push PCM out of the I2S port: resample to kCodecRate, expand mono to
/// stereo, apply software volume.
///
/// Resampling is linear interpolation. For the 22.05kHz narration that is an
/// exact 2x ratio so every other output frame is a real sample; it is plenty
/// for speech on a small speaker.
bool pushPcm(const int16_t* pcm, size_t frames, uint16_t channels,
             uint32_t src_rate) {
    if (!s_available || pcm == nullptr || frames == 0) return false;
    if (src_rate == 0) return false;
    if (!enable()) return false;

    // Pre-roll of silence. The codec and the DMA pipeline both need a moment
    // to settle, and without this the first few milliseconds of speech land
    // during that settling and are heard as a hiccup at the head of the clip.
    std::memset(s_chunk, 0, kChunkFrames * 2 * sizeof(int16_t));
    for (int i = 0; i < 3; ++i) {   // ~35ms at 44.1kHz
        size_t primed = 0;
        i2s_channel_write(s_tx, s_chunk, kChunkFrames * 2 * sizeof(int16_t),
                          &primed, 500);
    }

    const int32_t gain = static_cast<int32_t>(s_volume);
    // Fixed-point source position, 16.16.
    const uint64_t step = (static_cast<uint64_t>(src_rate) << 16) / kCodecRate;
    const uint64_t total_in = static_cast<uint64_t>(frames) << 16;

    uint64_t pos = 0;
    while (pos < total_in) {
        size_t n = 0;
        while (n < kChunkFrames && pos < total_in) {
            const size_t i0 = static_cast<size_t>(pos >> 16);
            const size_t i1 = (i0 + 1 < frames) ? i0 + 1 : i0;
            const int32_t frac = static_cast<int32_t>(pos & 0xFFFF);

            int32_t l, r;
            if (channels == 2) {
                const int32_t l0 = pcm[i0 * 2],     l1 = pcm[i1 * 2];
                const int32_t r0 = pcm[i0 * 2 + 1], r1 = pcm[i1 * 2 + 1];
                l = l0 + (((l1 - l0) * frac) >> 16);
                r = r0 + (((r1 - r0) * frac) >> 16);
            } else {
                const int32_t s0 = pcm[i0], s1 = pcm[i1];
                l = r = s0 + (((s1 - s0) * frac) >> 16);
            }

            l = (l * gain) >> 8;
            r = (r * gain) >> 8;
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;

            s_chunk[n * 2]     = static_cast<int16_t>(l);
            s_chunk[n * 2 + 1] = static_cast<int16_t>(r);
            ++n;
            pos += step;
        }
        if (n == 0) break;
        size_t written = 0;
        const esp_err_t err = i2s_channel_write(s_tx, s_chunk,
                                                n * 2 * sizeof(int16_t),
                                                &written, 2000);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "i2s write failed: %s", esp_err_to_name(err));
            return false;
        }
    }

    // A little trailing silence so the codec does not hold the last sample
    // and click when the channel stops.
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

/// Renders one queued clip. Lives on CPU1 so it overlaps the panel refresh
/// that blocks CPU0.
void playTask(void*) {
    PlayReq req{};
    for (;;) {
        if (xQueueReceive(s_play_q, &req, portMAX_DELAY) != pdTRUE) continue;
        s_playing = true;
        WavInfo info{};
        if (parseWavMemory(req.data, req.len, &info)) {
            const int16_t* pcm =
                reinterpret_cast<const int16_t*>(req.data + info.data_offset);
            const size_t frames =
                info.data_bytes / (sizeof(int16_t) * info.channels);
            ESP_LOGI(kTag, "play %s (%.2fs, %luHz, %s) [async]", req.label,
                     static_cast<float>(frames) /
                         static_cast<float>(info.sample_rate),
                     (unsigned long)info.sample_rate,
                     info.channels == 2 ? "stereo" : "mono");
            pushPcm(pcm, frames, info.channels, info.sample_rate);
        }
        s_playing = false;
    }
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
    std_cfg.clk_cfg = clockFor(kCodecRate);
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
    s_rate = kCodecRate;
    s_volume = kDefaultVolume;
    s_available = true;

    // Bring the channel up now and leave it up: enabling it lazily made the
    // first clip of each playback carry the start-up transient.
    if (!enable()) {
        ESP_LOGW(kTag, "channel did not enable at init; will retry on first clip");
    }

    s_play_q = xQueueCreate(2, sizeof(PlayReq));
    if (s_play_q == nullptr) {
        ESP_LOGE(kTag, "could not create the playback queue");
        return ESP_ERR_NO_MEM;
    }
    // CPU1: the whole point is to keep rendering audio while the panel
    // refresh blocks CPU0. 6KB is ample -- this task has no deep call chain
    // and no alloca (unlike the M5Unified task this replaces).
    if (xTaskCreatePinnedToCore(playTask, "audio_play", 6144, nullptr, 4,
                                &s_play_task, 1) != pdPASS) {
        ESP_LOGE(kTag, "could not start the playback task");
        return ESP_FAIL;
    }
    ESP_LOGI(kTag, "async playback task on CPU1 (overlaps the panel refresh)");

    ESP_LOGI(kTag, "I2S TX up @%luHz fixed (MCK=%d BCK=%d WS=%d DOUT=%d), "
                   "codec 0x%02X, vol %u",
             (unsigned long)kCodecRate, pins::kSpkMclk, pins::kSpkBclk,
             pins::kSpkWs, pins::kSpkData, kCodecAddr, s_volume);
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

    // Deliberately NOT disabling the channel: leaving it enabled avoids a
    // start/stop transient on every clip. auto_clear feeds zeros when we are
    // not writing, so an idle channel is silent.
    return pushPcm(pcm, frames, info.channels, info.sample_rate);
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

    return pushPcm(pcm, frames, info.channels, info.sample_rate);
}

bool preloadWavFile(const char* path) {
    if (!s_available || path == nullptr) return false;

    // Never touch the shared buffer while the task is reading from it.
    if (!waitIdle(5000)) {
        ESP_LOGW(kTag, "preload: previous clip still playing, stopping it");
        stop();
    }

    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGE(kTag, "cannot open %s", path);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 44 || !ensurePcm(static_cast<size_t>(size))) {
        std::fclose(f);
        return false;
    }
    const size_t got = std::fread(s_pcm, 1, static_cast<size_t>(size), f);
    std::fclose(f);
    if (got == 0) {
        ESP_LOGE(kTag, "read 0 bytes from %s", path);
        return false;
    }
    s_pcm_len = got;
    ESP_LOGD(kTag, "preloaded %u bytes from %s", (unsigned)got, path);
    return true;
}

bool playPreloadedAsync() {
    if (!s_available || s_play_q == nullptr || s_pcm_len == 0) return false;
    PlayReq req{};
    req.data = reinterpret_cast<const unsigned char*>(s_pcm);
    req.len = static_cast<unsigned int>(s_pcm_len);
    std::snprintf(req.label, sizeof(req.label), "preloaded");
    return xQueueSend(s_play_q, &req, pdMS_TO_TICKS(50)) == pdTRUE;
}

bool playMemoryAsync(const unsigned char* data, unsigned int len,
                     const char* label) {
    if (!s_available || s_play_q == nullptr || data == nullptr) return false;
    PlayReq req{};
    req.data = data;
    req.len = len;
    std::snprintf(req.label, sizeof(req.label), "%s", label ? label : "flash");
    return xQueueSend(s_play_q, &req, pdMS_TO_TICKS(50)) == pdTRUE;
}

bool waitIdle(uint32_t timeout_ms) {
    uint32_t waited = 0;
    while ((s_playing || uxQueueMessagesWaiting(s_play_q) > 0) &&
           waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    return waited < timeout_ms;
}

void tone(float freq_hz, uint32_t ms) {
    if (!s_available || ms == 0) return;
    constexpr uint32_t kToneRate = kCodecRate;
    const size_t frames = (kToneRate * ms) / 1000;
    if (!enable()) return;

    size_t done = 0;
    // Short silence lead-in, same reason as pushPcm().
    std::memset(s_chunk, 0, kChunkFrames * 2 * sizeof(int16_t));
    {
        size_t primed = 0;
        i2s_channel_write(s_tx, s_chunk, kChunkFrames * 2 * sizeof(int16_t),
                          &primed, 500);
    }

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
}

void chirp() { tone(1200.0f, 70); }

void bootChime() {
    tone(880.0f, 110);
    tone(1320.0f, 130);
}

void diagnose() {
    ESP_LOGW(kTag, "--- audio diagnostics ---");
    ESP_LOGW(kTag, "codec 0x%02X present: %s", kCodecAddr,
             M5.In_I2C.scanID(kCodecAddr, kCodecFreq) ? "yes" : "NO");
    ESP_LOGW(kTag, "codec_en GPIO%d=%d  spk_en GPIO%d=%d", kCodecEnPin,
             gpio_get_level(kCodecEnPin), kSpkEnPin, gpio_get_level(kSpkEnPin));

    // Read the registers back: if these do not match what we wrote, the I2C
    // writes are not sticking and no amount of I2S work will help.
    for (const auto& r : kCodecInit) {
        uint8_t got = 0xFF;
        const bool ok =
            M5.In_I2C.readRegister(kCodecAddr, r.reg, &got, 1, kCodecFreq);
        ESP_LOGW(kTag, "  reg 0x%02X: wrote 0x%02X, read %s0x%02X%s", r.reg, r.val,
                 ok ? "" : "(FAILED) ", got,
                 (ok && got == r.val) ? "  ok" : "  <-- MISMATCH");
    }

    ESP_LOGW(kTag, "I2S fixed rate %luHz, volume %u/255",
             (unsigned long)kCodecRate, s_volume);
    ESP_LOGW(kTag, "playing a 2s 1kHz tone at full volume now...");
    const uint8_t saved = s_volume;
    s_volume = 255;
    tone(1000.0f, 2000);
    s_volume = saved;
    ESP_LOGW(kTag, "--- end diagnostics ---");
}

bool isPlaying() {
    return s_playing || (s_play_q != nullptr && uxQueueMessagesWaiting(s_play_q) > 0);
}

void stop() {
    if (s_play_q != nullptr) xQueueReset(s_play_q);
    // The task finishes the chunk it is in; that is at most ~12ms.
    uint32_t waited = 0;
    while (s_playing && waited < 500) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
}

void setVolume(uint8_t v) { s_volume = v; }
uint8_t volume() { return s_volume; }

}  // namespace hal::audio
