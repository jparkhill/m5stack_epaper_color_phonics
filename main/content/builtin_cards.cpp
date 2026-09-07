/*
 * Cards compiled into the firmware image.
 *
 * Purpose: make the device fully exercisable with no microSD card inserted --
 * which is the normal state while the card is in a reader on the desk. They
 * stay in the regular rotation rather than being a failure-only fallback, so
 * the in-memory asset path gets used every session instead of rotting.
 *
 * The blobs come from EMBED_FILES in main/CMakeLists.txt. They are ordinary
 * generator output: PNGs already dithered to the panel's six inks, WAVs at
 * 22.05kHz 16-bit mono. See main/embedded/README.md.
 */
#include "content/deck.h"

#include <cstring>

// EMBED_FILES symbol naming: "embedded/apple.png" -> _binary_apple_png_start
extern const unsigned char apple_png_start[] asm("_binary_apple_png_start");
extern const unsigned char apple_png_end[]   asm("_binary_apple_png_end");
extern const unsigned char apple_wav_start[] asm("_binary_apple_wav_start");
extern const unsigned char apple_wav_end[]   asm("_binary_apple_wav_end");
extern const unsigned char cat_png_start[]   asm("_binary_cat_png_start");
extern const unsigned char cat_png_end[]     asm("_binary_cat_png_end");
extern const unsigned char cat_wav_start[]   asm("_binary_cat_wav_start");
extern const unsigned char cat_wav_end[]     asm("_binary_cat_wav_end");

namespace content {

namespace {

struct BuiltinSpec {
    const char* id;
    char letter;
    const char* display;
    int8_t span_start;
    int8_t span_len;
    const unsigned char* png_start;
    const unsigned char* png_end;
    const unsigned char* wav_start;
    const unsigned char* wav_end;
};

// span values must match tools/phonics_data.py for the same words:
//   apple -> letter A, grapheme "a" at [0,1]  -> "Apple"
//   cat   -> letter C, grapheme "c" at [0,1]  -> "Cat"
const BuiltinSpec kBuiltins[] = {
    {"builtin_a_apple", 'A', "Apple", 0, 1,
     apple_png_start, apple_png_end, apple_wav_start, apple_wav_end},
    {"builtin_c_cat", 'C', "Cat", 0, 1,
     cat_png_start, cat_png_end, cat_wav_start, cat_wav_end},
};

}  // namespace

size_t builtinCardCount() { return sizeof(kBuiltins) / sizeof(kBuiltins[0]); }

bool fillBuiltinCard(size_t index, Card* out) {
    if (out == nullptr || index >= builtinCardCount()) return false;
    const BuiltinSpec& b = kBuiltins[index];

    Card c{};
    std::snprintf(c.id, sizeof(c.id), "%s", b.id);
    c.letter = b.letter;
    std::snprintf(c.display, sizeof(c.display), "%s", b.display);
    c.span_start = b.span_start;
    c.span_len = b.span_len;
    // Labels only -- nothing opens these paths for a built-in card.
    std::snprintf(c.image, sizeof(c.image), "<flash>/%s.png", b.display);
    std::snprintf(c.audio, sizeof(c.audio), "<flash>/%s.wav", b.display);

    c.image_data = b.png_start;
    c.image_len = static_cast<unsigned int>(b.png_end - b.png_start);
    c.audio_data = b.wav_start;
    c.audio_len = static_cast<unsigned int>(b.wav_end - b.wav_start);

    *out = c;
    return true;
}

}  // namespace content
