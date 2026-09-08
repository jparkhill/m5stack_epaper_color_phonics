#include "content/deck.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#include <cJSON.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>

namespace content {
namespace {

constexpr const char* kTag = "deck";
constexpr size_t kMaxCards = 256;
constexpr size_t kMaxManifestBytes = 512u * 1024u;

Card* s_cards = nullptr;
uint16_t* s_order = nullptr;   // shuffled playlist of card indices
size_t s_count = 0;
size_t s_dropped = 0;
size_t s_order_pos = 0;
int s_current = -1;
bool s_loaded = false;
char s_stats[80] = {0};
char s_base_dir[96] = {0};
uint8_t s_per_letter[26] = {0};

// Absolute paths to the 26 shared letter clips, from the manifest's
// "letters" array. Empty string means the manifest did not supply one.
char s_letter_audio[26][112] = {};

Focus s_focus{0, 0, 'A'};

bool fileExists(const char* path) {
    struct stat st{};
    return stat(path, &st) == 0 && st.st_size > 0;
}

/// Directory portion of a path, e.g. "/sd/phonics/manifest.json" ->
/// "/sd/phonics". Card paths in the manifest are relative to this.
void deriveBaseDir(const char* manifest_path) {
    std::snprintf(s_base_dir, sizeof(s_base_dir), "%s", manifest_path);
    char* slash = std::strrchr(s_base_dir, '/');
    if (slash != nullptr) {
        *slash = '\0';
    } else {
        s_base_dir[0] = '\0';
    }
}

void joinPath(char* dst, size_t dst_size, const char* rel) {
    if (rel != nullptr && rel[0] == '/') {
        std::snprintf(dst, dst_size, "%s", rel);   // already absolute
    } else {
        std::snprintf(dst, dst_size, "%s/%s", s_base_dir, rel ? rel : "");
    }
}

char* readWholeFile(const char* path, size_t* out_len) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGE(kTag, "cannot open manifest %s", path);
        return nullptr;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    if (size <= 0 || static_cast<size_t>(size) > kMaxManifestBytes) {
        ESP_LOGE(kTag, "manifest size %ld is out of range", size);
        std::fclose(f);
        return nullptr;
    }

    char* buf = static_cast<char*>(
        heap_caps_malloc(static_cast<size_t>(size) + 1, MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
        ESP_LOGE(kTag, "PSRAM alloc of %ld bytes for manifest failed", size);
        std::fclose(f);
        return nullptr;
    }
    const size_t got = std::fread(buf, 1, static_cast<size_t>(size), f);
    std::fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

void reshuffle() {
    if (s_count == 0) return;
    for (size_t i = 0; i < s_count; ++i) s_order[i] = static_cast<uint16_t>(i);
    // Fisher-Yates using the hardware RNG.
    for (size_t i = s_count - 1; i > 0; --i) {
        const size_t j = esp_random() % (i + 1);
        const uint16_t t = s_order[i];
        s_order[i] = s_order[j];
        s_order[j] = t;
    }
    s_order_pos = 0;
    ESP_LOGD(kTag, "playlist reshuffled (%u cards)", (unsigned)s_count);
}

int letterIndex(char letter) {
    if (letter >= 'A' && letter <= 'Z') return letter - 'A';
    if (letter >= 'a' && letter <= 'z') return letter - 'a';
    return -1;
}

}  // namespace

namespace {

bool allocTable() {
    if (s_cards != nullptr && s_order != nullptr) return true;
    s_cards = static_cast<Card*>(
        heap_caps_calloc(kMaxCards, sizeof(Card), MALLOC_CAP_SPIRAM));
    s_order = static_cast<uint16_t*>(
        heap_caps_calloc(kMaxCards, sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    return s_cards != nullptr && s_order != nullptr;
}

void countCard(const Card& c) {
    const int li = letterIndex(c.letter);
    if (li >= 0 && s_per_letter[li] < 255) ++s_per_letter[li];
}

}  // namespace

esp_err_t begin() {
    s_loaded = false;
    s_count = 0;
    s_dropped = 0;
    s_current = -1;
    std::memset(s_per_letter, 0, sizeof(s_per_letter));

    if (!allocTable()) {
        ESP_LOGE(kTag, "PSRAM alloc for the card table failed");
        return ESP_ERR_NO_MEM;
    }

    // Built-in cards go in first so the deck is never empty, even with no SD.
    for (size_t i = 0; i < builtinCardCount(); ++i) {
        Card c{};
        if (!fillBuiltinCard(i, &c)) continue;
        s_cards[s_count++] = c;
        countCard(c);

        // Let the scheduler breathe. Without this the parse loop plus any
        // sampled stat() calls can hold CPU0 long enough to trip the task
        // watchdog.
        if ((s_count & 0x0F) == 0) vTaskDelay(1);
        ESP_LOGI(kTag, "built-in card %s (%s): %u B image, %u B audio",
                 c.id, c.display, c.image_len, c.audio_len);
    }
    s_loaded = s_count > 0;
    reshuffle();
    std::snprintf(s_stats, sizeof(s_stats), "%u built-in card(s)",
                  (unsigned)s_count);
    return ESP_OK;
}

esp_err_t load(const char* manifest_path, bool verify_assets) {
    if (manifest_path == nullptr) return ESP_ERR_INVALID_ARG;
    if (!allocTable()) return ESP_ERR_NO_MEM;
    const int64_t t_start = esp_timer_get_time();
    // NOTE: does not reset the table -- built-ins registered by begin() stay
    // in the rotation and the SD deck is appended to them.
    const size_t before = s_count;
    deriveBaseDir(manifest_path);

    size_t len = 0;
    char* json = readWholeFile(manifest_path, &len);
    if (json == nullptr) return ESP_ERR_NOT_FOUND;

    cJSON* root = cJSON_Parse(json);
    heap_caps_free(json);
    if (root == nullptr) {
        const char* err = cJSON_GetErrorPtr();
        ESP_LOGE(kTag, "manifest is not valid JSON near: %.32s", err ? err : "?");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON* voice = cJSON_GetObjectItemCaseSensitive(root, "voice");
    if (cJSON_IsString(voice)) {
        ESP_LOGI(kTag, "manifest voice: %s", voice->valuestring);
    }

    // --- v2: the shared per-letter clips ---
    const cJSON* letters = cJSON_GetObjectItemCaseSensitive(root, "letters");
    int n_letter_clips = 0;
    if (cJSON_IsArray(letters)) {
        const cJSON* li = nullptr;
        cJSON_ArrayForEach(li, letters) {
            const cJSON* j_l = cJSON_GetObjectItemCaseSensitive(li, "letter");
            const cJSON* j_a = cJSON_GetObjectItemCaseSensitive(li, "audio");
            if (!cJSON_IsString(j_l) || !cJSON_IsString(j_a)) continue;
            const int idx = letterIndex(j_l->valuestring[0]);
            if (idx < 0) continue;
            joinPath(s_letter_audio[idx], sizeof(s_letter_audio[idx]),
                     j_a->valuestring);
            if (verify_assets && !fileExists(s_letter_audio[idx])) {
                ESP_LOGW(kTag, "letter clip missing: %s", s_letter_audio[idx]);
                s_letter_audio[idx][0] = '\0';
                continue;
            }
            ++n_letter_clips;
        }
        ESP_LOGI(kTag, "%d/26 shared letter clips available", n_letter_clips);
    } else {
        ESP_LOGW(kTag, "manifest has no \"letters\" array -- stepping through "
                       "the letters of a word will be silent");
    }

    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "cards");
    if (!cJSON_IsArray(arr)) {
        ESP_LOGE(kTag, "manifest has no \"cards\" array");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        if (s_count >= kMaxCards) {
            ESP_LOGW(kTag, "manifest exceeds %u cards; ignoring the rest",
                     (unsigned)kMaxCards);
            break;
        }

        const cJSON* j_id      = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON* j_letter  = cJSON_GetObjectItemCaseSensitive(item, "letter");
        const cJSON* j_display = cJSON_GetObjectItemCaseSensitive(item, "display");
        const cJSON* j_span    = cJSON_GetObjectItemCaseSensitive(item, "span");
        const cJSON* j_image   = cJSON_GetObjectItemCaseSensitive(item, "image");
        const cJSON* j_audio   = cJSON_GetObjectItemCaseSensitive(item, "audio");

        if (!cJSON_IsString(j_letter) || !cJSON_IsString(j_display) ||
            !cJSON_IsString(j_image) || !cJSON_IsString(j_audio)) {
            ESP_LOGW(kTag, "skipping malformed card entry #%u", (unsigned)s_count);
            ++s_dropped;
            continue;
        }

        Card c{};
        std::snprintf(c.id, sizeof(c.id), "%s",
                      cJSON_IsString(j_id) ? j_id->valuestring : j_display->valuestring);
        c.letter = j_letter->valuestring[0];
        std::snprintf(c.display, sizeof(c.display), "%s", j_display->valuestring);

        c.span_start = 0;
        c.span_len = 0;
        if (cJSON_IsArray(j_span) && cJSON_GetArraySize(j_span) == 2) {
            const cJSON* a = cJSON_GetArrayItem(j_span, 0);
            const cJSON* b = cJSON_GetArrayItem(j_span, 1);
            if (cJSON_IsNumber(a) && cJSON_IsNumber(b)) {
                c.span_start = static_cast<int8_t>(a->valueint);
                c.span_len = static_cast<int8_t>(b->valueint);
            }
        }

        joinPath(c.image, sizeof(c.image), j_image->valuestring);
        joinPath(c.audio, sizeof(c.audio), j_audio->valuestring);

        // Verification is SAMPLED, not exhaustive.
        //
        // Checking every card cost 2 stat() calls x 260 cards = 520 stats on
        // a 20MHz SPI-mode SD card. That took tens of seconds AND ran in a
        // tight loop with no yielding, which starved the idle task and
        // tripped the 30s task watchdog -- the board died ~29s into boot.
        //
        // One card per letter is enough to catch the cases that actually
        // happen: a card that was never written, a half-copied directory, a
        // manifest from a different generation. An individually missing file
        // is handled gracefully at use time anyway (the image falls back to a
        // big letter, the audio logs and is skipped), so it does not justify
        // a slow boot.
        const int li_probe = letterIndex(c.letter);
        const bool sample = verify_assets && li_probe >= 0 &&
                            s_per_letter[li_probe] == 0;
        if (sample) {
            if (!fileExists(c.image)) {
                ESP_LOGW(kTag, "dropping %s: image missing (%s)", c.id, c.image);
                ++s_dropped;
                continue;
            }
            if (!fileExists(c.audio)) {
                ESP_LOGW(kTag, "dropping %s: audio missing (%s)", c.id, c.audio);
                ++s_dropped;
                continue;
            }
        }

        s_cards[s_count++] = c;
        countCard(c);

        // Let the scheduler breathe. Without this the parse loop plus any
        // sampled stat() calls can hold CPU0 long enough to trip the task
        // watchdog.
        if ((s_count & 0x0F) == 0) vTaskDelay(1);
    }

    cJSON_Delete(root);

    if (s_count == before) {
        ESP_LOGE(kTag, "manifest added no usable cards. Re-run "
                       "tools/provision_sd.sh.");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(kTag, "manifest added %u card(s) on top of %u built-in(s)",
             (unsigned)(s_count - before), (unsigned)before);

    size_t letters_with_cards = 0;
    for (const uint8_t n : s_per_letter) {
        if (n > 0) ++letters_with_cards;
    }

    std::snprintf(s_stats, sizeof(s_stats), "%u cards, %u letters%s",
                  (unsigned)s_count, (unsigned)letters_with_cards,
                  s_dropped ? " (some dropped)" : "");

    reshuffle();
    s_loaded = true;
    ESP_LOGI(kTag, "deck load took %.2f s (%s verification)",
             (esp_timer_get_time() - t_start) / 1e6,
             verify_assets ? "sampled" : "no");

    ESP_LOGI(kTag, "loaded %u cards across %u letters (%u dropped)",
             (unsigned)s_count, (unsigned)letters_with_cards,
             (unsigned)s_dropped);

    // Flag any letter that came up short of the intended five.
    for (int i = 0; i < 26; ++i) {
        if (s_per_letter[i] > 0 && s_per_letter[i] < 5) {
            ESP_LOGW(kTag, "letter %c has only %u card(s)", 'A' + i,
                     (unsigned)s_per_letter[i]);
        } else if (s_per_letter[i] == 0) {
            ESP_LOGW(kTag, "letter %c has NO cards", 'A' + i);
        }
    }
    return ESP_OK;
}

bool loaded() { return s_loaded; }
size_t cardCount() { return s_count; }
size_t droppedCount() { return s_dropped; }

size_t letterCount() {
    size_t n = 0;
    for (const uint8_t c : s_per_letter) {
        if (c > 0) ++n;
    }
    return n;
}

const char* stats() { return s_stats; }

const Card* current() {
    if (!s_loaded || s_current < 0) return nullptr;
    return &s_cards[s_current];
}

const Card* advance() {
    if (!s_loaded || s_count == 0) return nullptr;
    if (s_order_pos >= s_count) reshuffle();
    s_current = s_order[s_order_pos++];
    resetFocus();
    return &s_cards[s_current];
}

const Card* advanceRandom() {
    if (!s_loaded || s_count == 0) return nullptr;

    // Letters that actually have cards.
    uint8_t avail[26];
    int n_avail = 0;
    for (int i = 0; i < 26; ++i) {
        if (s_per_letter[i] > 0) avail[n_avail++] = static_cast<uint8_t>(i);
    }
    if (n_avail == 0) return nullptr;

    // Two attempts, so a repeat of the current card gets one re-roll rather
    // than looping forever when the deck is tiny (e.g. built-ins only).
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int li = avail[esp_random() % static_cast<uint32_t>(n_avail)];

        // Split this letter's cards by whether the grapheme is word-initial.
        uint16_t initial[16], other[16];
        int n_init = 0, n_other = 0;
        for (size_t i = 0; i < s_count; ++i) {
            if (letterIndex(s_cards[i].letter) != li) continue;
            if (s_cards[i].span_start == 0) {
                if (n_init < 16) initial[n_init++] = static_cast<uint16_t>(i);
            } else {
                if (n_other < 16) other[n_other++] = static_cast<uint16_t>(i);
            }
        }
        if (n_init == 0 && n_other == 0) continue;

        // Roll for the bias, then fall back if the chosen bucket is empty.
        const bool want_initial =
            (esp_random() % 1000u) < static_cast<uint32_t>(kInitialGraphemeBias * 1000.0f);
        const uint16_t* bucket = nullptr;
        int bucket_n = 0;
        if (want_initial && n_init > 0) {
            bucket = initial; bucket_n = n_init;
        } else if (!want_initial && n_other > 0) {
            bucket = other; bucket_n = n_other;
        } else if (n_init > 0) {
            bucket = initial; bucket_n = n_init;
        } else {
            bucket = other; bucket_n = n_other;
        }

        const int pick = bucket[esp_random() % static_cast<uint32_t>(bucket_n)];
        if (pick == s_current && s_count > 1) continue;   // re-roll a repeat
        s_current = pick;
        resetFocus();
        return &s_cards[pick];
    }

    // Both attempts landed on the current card; just advance the playlist.
    return advance();
}

const Card* advanceLetter() {
    if (!s_loaded || s_count == 0) return nullptr;
    const char cur = (s_current >= 0) ? s_cards[s_current].letter : 'Z';
    int li = letterIndex(cur);
    if (li < 0) li = 25;
    for (int step = 1; step <= 26; ++step) {
        const int next = (li + step) % 26;
        if (s_per_letter[next] > 0) {
            return selectLetter(static_cast<char>('A' + next), 0);
        }
    }
    return nullptr;
}

const Card* selectLetter(char letter, int nth) {
    if (!s_loaded) return nullptr;
    const int li = letterIndex(letter);
    if (li < 0 || s_per_letter[li] == 0) return nullptr;

    const int total = s_per_letter[li];
    const int want = ((nth % total) + total) % total;

    int seen = 0;
    for (size_t i = 0; i < s_count; ++i) {
        if (letterIndex(s_cards[i].letter) != li) continue;
        if (seen == want) {
            s_current = static_cast<int>(i);
            resetFocus();
            return &s_cards[i];
        }
        ++seen;
    }
    return nullptr;
}

Focus currentFocus() { return s_focus; }

void resetFocus() {
    const Card* c = current();
    if (c == nullptr) {
        s_focus = Focus{0, 0, 'A'};
        return;
    }
    s_focus.start = c->span_start;
    s_focus.len = c->span_len;
    // Uppercase the letter at the focus so it maps to a letter clip.
    const char ch = c->display[c->span_start];
    s_focus.letter = (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 32) : ch;
}

Focus advanceFocus() {
    const Card* c = current();
    if (c == nullptr) return s_focus;
    const int len = static_cast<int>(std::strlen(c->display));
    if (len <= 0) return s_focus;

    // Step past the whole current grapheme (2 chars for "QU"), then wrap.
    int next = s_focus.start + (s_focus.len > 0 ? s_focus.len : 1);
    if (next >= len) next = 0;

    s_focus.start = static_cast<int8_t>(next);
    s_focus.len = 1;                     // single letters once stepping
    const char ch = c->display[next];
    s_focus.letter = (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 32) : ch;
    return s_focus;
}

const char* letterAudioPath(char letter) {
    const int idx = letterIndex(letter);
    if (idx < 0 || s_letter_audio[idx][0] == '\0') return nullptr;
    return s_letter_audio[idx];
}

void verifyAllAssets() {
    if (!s_loaded) {
        ESP_LOGW(kTag, "deck not loaded");
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    int missing_img = 0, missing_aud = 0, missing_letters = 0;

    for (int i = 0; i < 26; ++i) {
        if (s_letter_audio[i][0] == '\0') continue;
        if (!fileExists(s_letter_audio[i])) {
            ESP_LOGW(kTag, "letter clip missing: %s", s_letter_audio[i]);
            ++missing_letters;
        }
    }
    for (size_t i = 0; i < s_count; ++i) {
        const Card& c = s_cards[i];
        if (c.isBuiltin()) continue;
        if (!fileExists(c.image)) {
            ESP_LOGW(kTag, "missing image: %s", c.image);
            ++missing_img;
        }
        if (!fileExists(c.audio)) {
            ESP_LOGW(kTag, "missing audio: %s", c.audio);
            ++missing_aud;
        }
        if ((i & 0x0F) == 0) vTaskDelay(1);
    }
    ESP_LOGI(kTag, "full verify: %u cards in %.2f s -- %d images, %d audio, "
                   "%d letter clips missing",
             (unsigned)s_count, (esp_timer_get_time() - t0) / 1e6, missing_img,
             missing_aud, missing_letters);
}

int cardsForLetter(char letter) {
    const int li = letterIndex(letter);
    return li < 0 ? 0 : s_per_letter[li];
}

}  // namespace content
