/*
 * Speaker output: ES8311 codec + I2S, driven directly.
 *
 * WHY NOT M5.Speaker:
 * M5Unified's Speaker_Class runs a background task whose stack it sizes as
 * `1280 + dma_buf_len * 4` bytes -- and then, inside that task, does
 * `alloca(dma_buf_len * sizeof(int32_t))`. The alloca grows exactly as fast as
 * the stack does, so headroom for the rest of the frame is a fixed ~1280
 * bytes regardless of configuration. On this board that tips over into
 *     "***ERROR*** A stack overflow in task spk_task has been detected"
 * and there is no exposed knob to fix it. So this module owns the codec and
 * the I2S channel instead, and M5.begin() is told internal_spk = false.
 *
 * Playback is SYNCHRONOUS: i2s_channel_write() is called from the caller's
 * task. The app is already happy to block while narrating, the app task has a
 * 16KB stack, and this removes an entire class of concurrency bug.
 *
 * Codec register sequence and the two enable GPIOs are taken from
 * M5Unified's _speaker_enabled_cb_papercolor() (MIT, (c) M5Stack).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_err.h>

namespace hal::audio {

esp_err_t init();

/// True once the codec answered on I2C and the I2S channel came up.
bool available();

/// Play a 16-bit PCM WAV from the filesystem. Blocks until finished.
bool playWavFile(const char* path);

// --- Asynchronous playback -------------------------------------------------
//
// A ~16s panel refresh blocks the app task, so playing narration after it
// means the child waits in silence and then hears the word. Playing it on a
// separate task pinned to CPU1 lets the sound run WHILE the picture develops,
// which removes essentially all of the perceived latency.
//
// The SD card shares SPI2 with the panel, so a file must be read into PSRAM
// BEFORE the refresh starts -- never streamed off the card during it. Hence
// the two-step preload/play split.

/// Two preload slots, because a card's narration is TWO clips played back to
/// back: the shared per-letter clip, then the per-word clip. Both must be off
/// the SD card and in PSRAM before the panel refresh starts, since the card
/// and the panel share SPI2.
enum class Slot : uint8_t { kLetter = 0, kWord = 1, kCount = 2 };

/// Read a WAV off the filesystem into the given slot. Blocks for the read.
bool preloadWavFile(Slot slot, const char* path);

/// Queue a preloaded slot for playback and return immediately.
bool playSlotAsync(Slot slot, const char* label);

/// Start playing an in-flash blob on the audio task and return immediately.
bool playMemoryAsync(const unsigned char* data, unsigned int len, const char* label);

/// Block until asynchronous playback finishes (or the timeout elapses).
bool waitIdle(uint32_t timeout_ms = 30000);

/// Play a 16-bit PCM WAV already in memory (an EMBED_FILES blob in flash).
/// No copy is made. Blocks until finished.
bool playWavMemory(const unsigned char* data, unsigned int len, const char* label);

// Kept for call-site clarity; playback is synchronous either way.
inline bool playWavFileBlocking(const char* path, uint32_t = 0) {
    return playWavFile(path);
}
inline bool playWavMemoryBlocking(const unsigned char* d, unsigned int n,
                                  const char* label, uint32_t = 0) {
    return playWavMemory(d, n, label);
}

/// Short square-ish blip: immediate acknowledgement of a button press while
/// the ~10s panel refresh gets under way.
void chirp();

/// Sine tone, blocking.
void tone(float freq_hz, uint32_t ms);

/// Two rising notes, used once at boot to prove the speaker works.
void bootChime();

/// Read back the codec's configuration registers and play a loud, long test
/// tone. Exists because a silent speaker looks identical to a working one
/// from the firmware's side: the I2S writes succeed either way.
void diagnose();

/// True while the audio task is rendering a clip.
bool isPlaying();
void stop();

void setVolume(uint8_t volume);   // 0-255, applied in software
uint8_t volume();

}  // namespace hal::audio
