// ft8cb_decode.cpp
// FT8 decoder with CB callsign support
// Reads 12000 Hz, 16-bit mono PCM from stdin (15 seconds = 180000 samples)
// Outputs decoded FT8 messages as JSON lines on stdout
//
// Usage: ft8cb_decode [--wav FILE | --raw FILE]
//   (no args) = read raw 16-bit PCM from stdin
//   --wav FILE  = read WAV file
//   --raw FILE  = read raw 16-bit PCM file

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>

extern "C" {
#include <ft8/decode.h>
#include <ft8/message.h>
#include <ft8/constants.h>
#include <common/monitor.h>
#include <common/wave.h>
}

// ──────────────────────────────────────────────────────────────────────────────
// CB callsign validation: formats like 14FR001, 1AT106, 26AT715
// Pattern: 1-3 digits + 1-2 letters + 1-4 digits
// Special: 4-digit suffix only allowed with 1-digit country prefix
// ──────────────────────────────────────────────────────────────────────────────
static bool is_cb_callsign(const char* callsign)
{
    if (!callsign) return false;
    int i = 0;
    // 1-3 digit prefix
    int prefix_digits = 0;
    while (callsign[i] >= '0' && callsign[i] <= '9' && prefix_digits < 3) { i++; prefix_digits++; }
    if (prefix_digits == 0) return false;
    // 1-2 letter middle
    int letters = 0;
    while (callsign[i] >= 'A' && callsign[i] <= 'Z' && letters < 2) { i++; letters++; }
    if (letters == 0) return false;
    // 1-4 digit suffix
    int suffix_digits = 0;
    while (callsign[i] >= '0' && callsign[i] <= '9' && suffix_digits < 4) { i++; suffix_digits++; }
    if (suffix_digits == 0) return false;
    if (callsign[i] != '\0') return false;
    // 4-digit suffix only allowed with 1-digit prefix
    if (suffix_digits == 4 && prefix_digits != 1) return false;
    return true;
}

// ──────────────────────────────────────────────────────────────────────────────
// Callsign hash table (from decode_ft8.c example)
// ──────────────────────────────────────────────────────────────────────────────
#define CALLSIGN_HASHTABLE_SIZE 256

static struct {
    char callsign[12];
    uint32_t hash;
} callsign_hashtable[CALLSIGN_HASHTABLE_SIZE];

static int callsign_hashtable_size = 0;

static void hashtable_init() {
    callsign_hashtable_size = 0;
    memset(callsign_hashtable, 0, sizeof(callsign_hashtable));
}

static void hashtable_cleanup(uint8_t max_age) {
    for (int i = 0; i < CALLSIGN_HASHTABLE_SIZE; ++i) {
        if (callsign_hashtable[i].callsign[0] != '\0') {
            uint8_t age = (uint8_t)(callsign_hashtable[i].hash >> 24);
            if (age > max_age) {
                callsign_hashtable[i].callsign[0] = '\0';
                callsign_hashtable[i].hash = 0;
                callsign_hashtable_size--;
            } else {
                callsign_hashtable[i].hash = (((uint32_t)age + 1u) << 24) | (callsign_hashtable[i].hash & 0x3FFFFFu);
            }
        }
    }
}

static void hashtable_add(const char* callsign, uint32_t hash) {
    uint16_t hash10 = (hash >> 12) & 0x3FFu;
    int idx = (hash10 * 23) % CALLSIGN_HASHTABLE_SIZE;
    while (callsign_hashtable[idx].callsign[0] != '\0') {
        if (((callsign_hashtable[idx].hash & 0x3FFFFFu) == hash) &&
            (0 == strcmp(callsign_hashtable[idx].callsign, callsign))) {
            callsign_hashtable[idx].hash &= 0x3FFFFFu;
            return;
        }
        idx = (idx + 1) % CALLSIGN_HASHTABLE_SIZE;
    }
    callsign_hashtable_size++;
    strncpy(callsign_hashtable[idx].callsign, callsign, 11);
    callsign_hashtable[idx].callsign[11] = '\0';
    callsign_hashtable[idx].hash = hash;
}

static bool hashtable_lookup(ftx_callsign_hash_type_t hash_type, uint32_t hash, char* callsign) {
    uint8_t hash_shift = (hash_type == FTX_CALLSIGN_HASH_10_BITS) ? 12 :
                         (hash_type == FTX_CALLSIGN_HASH_12_BITS ? 10 : 0);
    uint16_t hash10 = (hash >> (12 - hash_shift)) & 0x3FFu;
    int idx = (hash10 * 23) % CALLSIGN_HASHTABLE_SIZE;
    while (callsign_hashtable[idx].callsign[0] != '\0') {
        if (((callsign_hashtable[idx].hash & 0x3FFFFFu) >> hash_shift) == hash) {
            strcpy(callsign, callsign_hashtable[idx].callsign);
            return true;
        }
        idx = (idx + 1) % CALLSIGN_HASHTABLE_SIZE;
    }
    callsign[0] = '\0';
    return false;
}

static ftx_callsign_hash_interface_t hash_if = {
    .lookup_hash = hashtable_lookup,
    .save_hash   = hashtable_add
};

// ──────────────────────────────────────────────────────────────────────────────
// JSON escape helper
// ──────────────────────────────────────────────────────────────────────────────
static void json_escape(char* out, const char* in, int max_len) {
    int j = 0;
    for (int i = 0; in[i] && j < max_len - 2; i++) {
        if (in[i] == '"' || in[i] == '\\') {
            out[j++] = '\\';
        }
        out[j++] = in[i];
    }
    out[j] = '\0';
}

// ──────────────────────────────────────────────────────────────────────────────
// FT8 decode pass
// ──────────────────────────────────────────────────────────────────────────────
const int kMin_score         = 10;
const int kMax_candidates    = 140;
const int kLDPC_iterations   = 25;
const int kMax_decoded       = 50;
const int kFreq_osr          = 2;
const int kTime_osr          = 2;

static void decode_and_print(const monitor_t* mon, struct tm* ts)
{
    const ftx_waterfall_t* wf = &mon->wf;

    ftx_candidate_t candidates[kMax_candidates];
    int num_candidates = ftx_find_candidates(wf, kMax_candidates, candidates, kMin_score);

    ftx_message_t decoded[kMax_decoded];
    ftx_message_t* decoded_ht[kMax_decoded];
    for (int i = 0; i < kMax_decoded; i++) decoded_ht[i] = NULL;

    int num_decoded = 0;

    for (int ci = 0; ci < num_candidates; ci++) {
        const ftx_candidate_t* cand = &candidates[ci];

        float freq_hz  = (mon->min_bin + cand->freq_offset + (float)cand->freq_sub / wf->freq_osr) / mon->symbol_period;
        float time_sec = (cand->time_offset + (float)cand->time_sub / wf->time_osr) * mon->symbol_period;

        ftx_message_t message;
        ftx_decode_status_t status;
        if (!ftx_decode_candidate(wf, cand, kLDPC_iterations, &message, &status))
            continue;

        // Check for duplicates
        int idx_hash = message.hash % kMax_decoded;
        bool found_empty = false, found_dup = false;
        do {
            if (decoded_ht[idx_hash] == NULL) {
                found_empty = true;
            } else if ((decoded_ht[idx_hash]->hash == message.hash) &&
                       (0 == memcmp(decoded_ht[idx_hash]->payload, message.payload, sizeof(message.payload)))) {
                found_dup = true;
            } else {
                idx_hash = (idx_hash + 1) % kMax_decoded;
            }
        } while (!found_empty && !found_dup);

        if (found_empty) {
            memcpy(&decoded[idx_hash], &message, sizeof(message));
            decoded_ht[idx_hash] = &decoded[idx_hash];
            num_decoded++;

            char text[FTX_MAX_MESSAGE_LENGTH];
            ftx_message_offsets_t offsets;
            ftx_message_rc_t rc = ftx_message_decode(&message, &hash_if, text, &offsets);
            if (rc != FTX_MESSAGE_RC_OK) {
                snprintf(text, sizeof(text), "?ERR[%d]", (int)rc);
            }

            float snr = cand->score * 0.5f;

            // Check if any token is a CB callsign
            bool has_cb = false;
            char tok1[16] = {}, tok2[16] = {}, tok3[16] = {};
            sscanf(text, "%15s %15s %15s", tok1, tok2, tok3);
            if (is_cb_callsign(tok1) || is_cb_callsign(tok2) || is_cb_callsign(tok3))
                has_cb = true;

            // JSON escape text
            char text_escaped[256];
            json_escape(text_escaped, text, sizeof(text_escaped));

            // Format timestamp
            char ts_buf[20] = "000000";
            if (ts && (ts->tm_hour || ts->tm_min || ts->tm_sec))
                snprintf(ts_buf, sizeof(ts_buf), "%02d%02d%02d",
                         ts->tm_hour, ts->tm_min, ts->tm_sec);

            printf("{\"ts\":\"%s\",\"snr\":%.1f,\"dt\":%.2f,\"freq\":%.0f,\"msg\":\"%s\",\"cb\":%s}\n",
                   ts_buf, snr, time_sec, freq_hz, text_escaped,
                   has_cb ? "true" : "false");
            fflush(stdout);
        }
    }
    fprintf(stderr, "[ft8cb_decode] Decoded %d messages\n", num_decoded);
}

// ──────────────────────────────────────────────────────────────────────────────
// Read raw 16-bit PCM from FILE* (stdin or file) into float array
// Returns number of samples read
// ──────────────────────────────────────────────────────────────────────────────
static int read_raw_pcm(FILE* f, float* signal, int max_samples)
{
    int count = 0;
    int16_t sample;
    while (count < max_samples && fread(&sample, sizeof(int16_t), 1, f) == 1) {
        signal[count++] = sample / 32768.0f;
    }
    return count;
}

// ──────────────────────────────────────────────────────────────────────────────
// Main
// ──────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv)
{
    const char* wav_path = nullptr;
    const char* raw_path = nullptr;
    bool read_stdin = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc) {
            wav_path = argv[++i];
            read_stdin = false;
        } else if (strcmp(argv[i], "--raw") == 0 && i + 1 < argc) {
            raw_path = argv[++i];
            read_stdin = false;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "Usage: ft8cb_decode [--wav FILE|--raw FILE]\n");
            fprintf(stderr, "  (no args) reads raw 16-bit PCM at 12kHz from stdin\n");
            return 0;
        }
    }

    const int sample_rate = 12000;
    const float slot_period = FT8_SLOT_TIME; // 15.0 seconds
    int num_samples = (int)(slot_period * sample_rate);
    float* signal = (float*)malloc(num_samples * sizeof(float));
    if (!signal) { fprintf(stderr, "[ft8cb_decode] malloc failed\n"); return 1; }

    // Load audio
    if (wav_path) {
        int rc = load_wav(signal, &num_samples, (int*)&sample_rate, wav_path);
        if (rc < 0) {
            fprintf(stderr, "[ft8cb_decode] Cannot load WAV: %s\n", wav_path);
            free(signal);
            return 1;
        }
        fprintf(stderr, "[ft8cb_decode] Loaded WAV: %d samples @ %d Hz\n", num_samples, sample_rate);
    } else if (raw_path) {
        FILE* f = fopen(raw_path, "rb");
        if (!f) { fprintf(stderr, "[ft8cb_decode] Cannot open: %s\n", raw_path); free(signal); return 1; }
        num_samples = read_raw_pcm(f, signal, num_samples);
        fclose(f);
        fprintf(stderr, "[ft8cb_decode] Read %d raw samples from %s\n", num_samples, raw_path);
    } else {
        // Read from stdin
        num_samples = read_raw_pcm(stdin, signal, num_samples);
        fprintf(stderr, "[ft8cb_decode] Read %d raw samples from stdin\n", num_samples);
    }

    if (num_samples < 1000) {
        fprintf(stderr, "[ft8cb_decode] Not enough samples (%d)\n", num_samples);
        free(signal);
        return 1;
    }

    // Set up monitor
    monitor_config_t cfg = {
        .f_min       = 200.0f,
        .f_max       = 3000.0f,
        .sample_rate = sample_rate,
        .time_osr    = kTime_osr,
        .freq_osr    = kFreq_osr,
        .protocol    = FTX_PROTOCOL_FT8
    };

    hashtable_init();
    monitor_t mon;
    monitor_init(&mon, &cfg);

    // Process frames
    for (int pos = 0; pos + mon.block_size <= num_samples; pos += mon.block_size) {
        monitor_process(&mon, signal + pos);
    }
    fprintf(stderr, "[ft8cb_decode] Waterfall: %d symbols, max_mag=%.1f dB\n",
            mon.wf.num_blocks, mon.max_mag);

    // Decode
    struct tm ts_zero = {};
    decode_and_print(&mon, &ts_zero);

    monitor_free(&mon);
    hashtable_cleanup(10);
    free(signal);

    return 0;
}
