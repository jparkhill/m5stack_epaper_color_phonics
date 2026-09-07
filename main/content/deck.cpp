#include "content/deck.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#include <cJSON.h>
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

        if (verify_assets) {
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
    }

    cJSON_Delete(root);

    if (s_count == before) {
        ESP_LOGE(kTag, "manifest added no usable cards. Re-run "
                       "tools/provision_sd.sh.");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(kTag, "manifest added %u card(s) on top of %u built-in(s)",
             (unsigned)(s_count - before), (unsigned)before);

    size_t letters = 0;
    for (const uint8_t n : s_per_letter) {
        if (n > 0) ++letters;
    }

    std::snprintf(s_stats, sizeof(s_stats), "%u cards, %u letters%s",
                  (unsigned)s_count, (unsigned)letters,
                  s_dropped ? " (some dropped)" : "");

    reshuffle();
    s_loaded = true;

    ESP_LOGI(kTag, "loaded %u cards across %u letters (%u dropped)",
             (unsigned)s_count, (unsigned)letters, (unsigned)s_dropped);

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
    return &s_cards[s_current];
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
            return &s_cards[i];
        }
        ++seen;
    }
    return nullptr;
}

int cardsForLetter(char letter) {
    const int li = letterIndex(letter);
    return li < 0 ? 0 : s_per_letter[li];
}

}  // namespace content
