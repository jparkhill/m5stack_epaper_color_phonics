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

// All 26 shared letter clips, so the taught letter can be stepped through a
// word with no microSD card present. ~4MB of a 15MB partition; see
// main/embedded/README.md for why that trade is worth it.
extern const unsigned char ltr_a_wav_start[] asm("_binary_ltr_a_wav_start");
extern const unsigned char ltr_a_wav_end[]   asm("_binary_ltr_a_wav_end");
extern const unsigned char ltr_b_wav_start[] asm("_binary_ltr_b_wav_start");
extern const unsigned char ltr_b_wav_end[]   asm("_binary_ltr_b_wav_end");
extern const unsigned char ltr_c_wav_start[] asm("_binary_ltr_c_wav_start");
extern const unsigned char ltr_c_wav_end[]   asm("_binary_ltr_c_wav_end");
extern const unsigned char ltr_d_wav_start[] asm("_binary_ltr_d_wav_start");
extern const unsigned char ltr_d_wav_end[]   asm("_binary_ltr_d_wav_end");
extern const unsigned char ltr_e_wav_start[] asm("_binary_ltr_e_wav_start");
extern const unsigned char ltr_e_wav_end[]   asm("_binary_ltr_e_wav_end");
extern const unsigned char ltr_f_wav_start[] asm("_binary_ltr_f_wav_start");
extern const unsigned char ltr_f_wav_end[]   asm("_binary_ltr_f_wav_end");
extern const unsigned char ltr_g_wav_start[] asm("_binary_ltr_g_wav_start");
extern const unsigned char ltr_g_wav_end[]   asm("_binary_ltr_g_wav_end");
extern const unsigned char ltr_h_wav_start[] asm("_binary_ltr_h_wav_start");
extern const unsigned char ltr_h_wav_end[]   asm("_binary_ltr_h_wav_end");
extern const unsigned char ltr_i_wav_start[] asm("_binary_ltr_i_wav_start");
extern const unsigned char ltr_i_wav_end[]   asm("_binary_ltr_i_wav_end");
extern const unsigned char ltr_j_wav_start[] asm("_binary_ltr_j_wav_start");
extern const unsigned char ltr_j_wav_end[]   asm("_binary_ltr_j_wav_end");
extern const unsigned char ltr_k_wav_start[] asm("_binary_ltr_k_wav_start");
extern const unsigned char ltr_k_wav_end[]   asm("_binary_ltr_k_wav_end");
extern const unsigned char ltr_l_wav_start[] asm("_binary_ltr_l_wav_start");
extern const unsigned char ltr_l_wav_end[]   asm("_binary_ltr_l_wav_end");
extern const unsigned char ltr_m_wav_start[] asm("_binary_ltr_m_wav_start");
extern const unsigned char ltr_m_wav_end[]   asm("_binary_ltr_m_wav_end");
extern const unsigned char ltr_n_wav_start[] asm("_binary_ltr_n_wav_start");
extern const unsigned char ltr_n_wav_end[]   asm("_binary_ltr_n_wav_end");
extern const unsigned char ltr_o_wav_start[] asm("_binary_ltr_o_wav_start");
extern const unsigned char ltr_o_wav_end[]   asm("_binary_ltr_o_wav_end");
extern const unsigned char ltr_p_wav_start[] asm("_binary_ltr_p_wav_start");
extern const unsigned char ltr_p_wav_end[]   asm("_binary_ltr_p_wav_end");
extern const unsigned char ltr_q_wav_start[] asm("_binary_ltr_q_wav_start");
extern const unsigned char ltr_q_wav_end[]   asm("_binary_ltr_q_wav_end");
extern const unsigned char ltr_r_wav_start[] asm("_binary_ltr_r_wav_start");
extern const unsigned char ltr_r_wav_end[]   asm("_binary_ltr_r_wav_end");
extern const unsigned char ltr_s_wav_start[] asm("_binary_ltr_s_wav_start");
extern const unsigned char ltr_s_wav_end[]   asm("_binary_ltr_s_wav_end");
extern const unsigned char ltr_t_wav_start[] asm("_binary_ltr_t_wav_start");
extern const unsigned char ltr_t_wav_end[]   asm("_binary_ltr_t_wav_end");
extern const unsigned char ltr_u_wav_start[] asm("_binary_ltr_u_wav_start");
extern const unsigned char ltr_u_wav_end[]   asm("_binary_ltr_u_wav_end");
extern const unsigned char ltr_v_wav_start[] asm("_binary_ltr_v_wav_start");
extern const unsigned char ltr_v_wav_end[]   asm("_binary_ltr_v_wav_end");
extern const unsigned char ltr_w_wav_start[] asm("_binary_ltr_w_wav_start");
extern const unsigned char ltr_w_wav_end[]   asm("_binary_ltr_w_wav_end");
extern const unsigned char ltr_x_wav_start[] asm("_binary_ltr_x_wav_start");
extern const unsigned char ltr_x_wav_end[]   asm("_binary_ltr_x_wav_end");
extern const unsigned char ltr_y_wav_start[] asm("_binary_ltr_y_wav_start");
extern const unsigned char ltr_y_wav_end[]   asm("_binary_ltr_y_wav_end");
extern const unsigned char ltr_z_wav_start[] asm("_binary_ltr_z_wav_start");
extern const unsigned char ltr_z_wav_end[]   asm("_binary_ltr_z_wav_end");

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

namespace {

struct LetterClip {
    const unsigned char* start;
    const unsigned char* end;
};

const LetterClip kLetterClips[26] = {
    {ltr_a_wav_start, ltr_a_wav_end},
    {ltr_b_wav_start, ltr_b_wav_end},
    {ltr_c_wav_start, ltr_c_wav_end},
    {ltr_d_wav_start, ltr_d_wav_end},
    {ltr_e_wav_start, ltr_e_wav_end},
    {ltr_f_wav_start, ltr_f_wav_end},
    {ltr_g_wav_start, ltr_g_wav_end},
    {ltr_h_wav_start, ltr_h_wav_end},
    {ltr_i_wav_start, ltr_i_wav_end},
    {ltr_j_wav_start, ltr_j_wav_end},
    {ltr_k_wav_start, ltr_k_wav_end},
    {ltr_l_wav_start, ltr_l_wav_end},
    {ltr_m_wav_start, ltr_m_wav_end},
    {ltr_n_wav_start, ltr_n_wav_end},
    {ltr_o_wav_start, ltr_o_wav_end},
    {ltr_p_wav_start, ltr_p_wav_end},
    {ltr_q_wav_start, ltr_q_wav_end},
    {ltr_r_wav_start, ltr_r_wav_end},
    {ltr_s_wav_start, ltr_s_wav_end},
    {ltr_t_wav_start, ltr_t_wav_end},
    {ltr_u_wav_start, ltr_u_wav_end},
    {ltr_v_wav_start, ltr_v_wav_end},
    {ltr_w_wav_start, ltr_w_wav_end},
    {ltr_x_wav_start, ltr_x_wav_end},
    {ltr_y_wav_start, ltr_y_wav_end},
    {ltr_z_wav_start, ltr_z_wav_end},
};

}  // namespace

bool embeddedLetterClip(char letter, const unsigned char** data,
                        unsigned int* len) {
    if (data == nullptr || len == nullptr) return false;
    int idx = -1;
    if (letter >= 'A' && letter <= 'Z') idx = letter - 'A';
    else if (letter >= 'a' && letter <= 'z') idx = letter - 'a';
    if (idx < 0) return false;
    const LetterClip& c = kLetterClips[idx];
    *data = c.start;
    *len = static_cast<unsigned int>(c.end - c.start);
    return *len > 44;
}

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
