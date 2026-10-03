/* Direct N64 B1/S1 -> AFB/AFX/AFC/AFV authoring tool.
 *
 * It deliberately stops at the common authoring IR: B1 lowers to explicit
 * samples/zones and CSeq lowers to notes.  The shared optimizer and emitter
 * own every final AFB/AFX layout decision.
 */
#include "afx_compile_c.h"
#include "afx_n64_cseq.h"
#include "afx_sample_c.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int n64_vadpcm_decode(const uint8_t *data, int data_len, const int16_t *coef,
                      int predictors, int16_t *out);

typedef struct { const uint8_t *control, *table; uint32_t control_bytes, table_bytes, bank, rate, percussion; int instruments; } bank_t;
typedef struct { uint32_t wavetable; afx_c_sample_t sample; uint8_t *data; } cached_sample_t;
typedef struct { uint32_t envelope, keymap, wavetable; uint8_t pan, volume; } sound_t;

static uint16_t be16(const uint8_t *p) { return (uint16_t)p[0] << 8 | p[1]; }
static int16_t sbe16(const uint8_t *p) { return (int16_t)be16(p); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int32_t sbe32(const uint8_t *p) { return (int32_t)be32(p); }
static int within(uint32_t at, uint32_t bytes, uint32_t size) { return at <= bytes && size <= bytes - at; }
static uint32_t rounded_ratio(uint32_t value, uint32_t numerator, uint32_t denominator) {
    return (uint32_t)(((uint64_t)value * numerator + denominator / 2u) / denominator);
}

static int read_file(const char *path, uint8_t **out, uint32_t *out_bytes) {
    FILE *file = fopen(path, "rb"); long size; uint8_t *data = NULL;
    if (!file || fseek(file, 0, SEEK_END) || (size = ftell(file)) <= 0 || (uint64_t)size > UINT32_MAX ||
        fseek(file, 0, SEEK_SET) || !(data = malloc((size_t)size)) || fread(data, 1, (size_t)size, file) != (size_t)size) {
        if (file) fclose(file); free(data); return -1;
    }
    fclose(file); *out = data; *out_bytes = (uint32_t)size; return 0;
}

static int write_file(const char *path, const uint8_t *data, uint32_t bytes) {
    FILE *file = fopen(path, "wb");
    int failed = !file || fwrite(data, 1, bytes, file) != bytes || fclose(file);
    return failed ? -1 : 0;
}

static char *with_suffix(const char *path, const char *suffix) {
    const char *dot = strrchr(path, '.'); size_t head = dot ? (size_t)(dot - path) : strlen(path), tail = strlen(suffix);
    char *result = malloc(head + tail + 1u);
    if (!result) return NULL;
    memcpy(result, path, head); memcpy(result + head, suffix, tail + 1u); return result;
}

static int bank_open(bank_t *out, const uint8_t *control, uint32_t control_bytes,
                     const uint8_t *table, uint32_t table_bytes, uint32_t index) {
    if (!out || control_bytes < 8 || control[0] != 'B' || control[1] != '1' || index >= be16(control + 2) ||
        !within(4u + index * 4u, control_bytes, 4)) return -1;
    uint32_t bank = be32(control + 4u + index * 4u);
    if (!within(bank, control_bytes, 12)) return -1;
    int instruments = sbe16(control + bank);
    uint32_t rate = be32(control + bank + 4u);
    if (instruments < 1 || instruments > 128 || rate < 4000 || rate > 96000 ||
        !within(bank + 12u, control_bytes, (uint32_t)instruments * 4u)) return -1;
    *out = (bank_t){control, table, control_bytes, table_bytes, bank, rate, be32(control + bank + 8u), instruments}; return 0;
}

static int bank_instrument(const bank_t *bank, uint8_t program, int percussion, uint32_t *out) {
    uint32_t at = percussion && bank->percussion ? bank->percussion :
                  (program < bank->instruments ? be32(bank->control + bank->bank + 12u + program * 4u) : 0);
    if (!at) for (int index = 0; index < bank->instruments && !at; ++index)
        at = be32(bank->control + bank->bank + 12u + (uint32_t)index * 4u);
    if (!within(at, bank->control_bytes, 16) || sbe16(bank->control + at + 14u) < 1 ||
        sbe16(bank->control + at + 14u) > 1024 ||
        !within(at + 16u, bank->control_bytes, (uint32_t)sbe16(bank->control + at + 14u) * 4u)) return -1;
    *out = at; return 0;
}

static int select_sound(const bank_t *bank, uint32_t instrument, uint8_t key, uint8_t velocity,
                        sound_t *out, uint8_t *instrument_volume, uint8_t *instrument_pan) {
    const uint8_t *control = bank->control; int count = sbe16(control + instrument + 14u); uint32_t found = 0;
    *instrument_volume = control[instrument]; *instrument_pan = control[instrument + 1u];
    for (int index = 0; index < count; ++index) {
        uint32_t at = be32(control + instrument + 16u + (uint32_t)index * 4u);
        if (!within(at, bank->control_bytes, 16)) return -1;
        uint32_t envelope = be32(control + at), keymap = be32(control + at + 4u), wavetable = be32(control + at + 8u);
        if (!within(envelope, bank->control_bytes, 16) || !within(keymap, bank->control_bytes, 6) ||
            !within(wavetable, bank->control_bytes, 20)) return -1;
        if (velocity < control[keymap] || velocity > control[keymap + 1u] || key < control[keymap + 2u] || key > control[keymap + 3u]) continue;
        if (found++) return -1;
        *out = (sound_t){envelope, keymap, wavetable, control[at + 12u], control[at + 13u]};
    }
    return found == 1 ? 0 : 1; /* 1 is libaudio's deliberately silent placeholder. */
}

static int make_pcm16(const bank_t *bank, uint32_t wavetable, uint8_t **out, uint32_t *out_frames,
                      uint32_t *out_loop_start, uint32_t *out_loop_end, int *out_loop) {
    const uint8_t *control = bank->control; const uint8_t *wave = control + wavetable;
    uint32_t start = be32(wave), length = (uint32_t)sbe32(wave + 4u), loop_at = be32(wave + 12u);
    uint8_t *pcm = NULL;
    if (!length || !within(start, bank->table_bytes, length)) return -1;
    if (wave[8] == 1) {
        if (length & 1u) return -1;
        pcm = malloc(length); if (!pcm) return -1;
        for (uint32_t i = 0; i < length; i += 2) { pcm[i] = bank->table[start + i + 1u]; pcm[i + 1u] = bank->table[start + i]; }
    } else if (wave[8] == 0) {
        uint32_t book = be32(wave + 16u); int order, predictors;
        if (!within(book, bank->control_bytes, 8)) return -1;
        order = sbe32(control + book); predictors = sbe32(control + book + 4u);
        if (order != 2 || predictors < 1 || predictors > 16 || !within(book + 8u, bank->control_bytes, (uint32_t)predictors * 16u)) return -1;
        length = length / 9u * 9u; if (!length) return -1;
        pcm = malloc((size_t)(length / 9u) * 32u); if (!pcm) return -1;
        int16_t coefficients[256];
        for (int i = 0; i < predictors * 16; ++i) coefficients[i] = sbe16(control + book + 8u + (uint32_t)i * 2u);
        if (n64_vadpcm_decode(bank->table + start, (int)length, coefficients, predictors, (int16_t *)pcm)) { free(pcm); return -1; }
        length = length / 9u * 32u;
    } else return -1;
    uint32_t frames = length / 2u;
    *out_loop = 0; *out_loop_start = 0; *out_loop_end = frames - 1u;
    if (loop_at) {
        if (!within(loop_at, bank->control_bytes, 12)) { free(pcm); return -1; }
        uint32_t first = be32(control + loop_at), end = be32(control + loop_at + 4u), count = be32(control + loop_at + 8u);
        if (first >= end || end > frames) { free(pcm); return -1; }
        if (count) { *out_loop = 1; *out_loop_start = first; *out_loop_end = end; }
    }
    *out = pcm; *out_frames = frames; return 0;
}

static int cache_sample(const bank_t *bank, uint32_t wavetable, cached_sample_t **items,
                        uint32_t *count, uint32_t *capacity, const afx_c_sample_t **out) {
    for (uint32_t i = 0; i < *count; ++i) if ((*items)[i].wavetable == wavetable) { *out = &(*items)[i].sample; return 0; }
    if (*count == *capacity) {
        uint32_t next = *capacity ? *capacity * 2u : 32u;
        cached_sample_t *grown = realloc(*items, (size_t)next * sizeof(**items));
        if (!grown) return -1;
        *items = grown; *capacity = next;
    }
    uint8_t *source = NULL, *resampled = NULL, *encoded = NULL; uint32_t source_frames, frames, loop_start, loop_end; int looping;
    if (make_pcm16(bank, wavetable, &source, &source_frames, &loop_start, &loop_end, &looping)) return -1;
    uint32_t rate = bank->rate;
    if (source_frames > 65535u) rate = (uint32_t)((uint64_t)65535u * bank->rate / source_frames);
    if (rate < 4000u || afx_c_resample_pcm16(source, source_frames, bank->rate, rate, &resampled, &frames) || !frames || frames > 65535u) goto failed;
    if (looping) {
        loop_start = rounded_ratio(loop_start, rate, bank->rate); loop_end = rounded_ratio(loop_end, rate, bank->rate);
        if (loop_start >= loop_end || loop_end > frames) goto failed;
        --loop_end; /* B1 loop end is exclusive; AICA LEA is inclusive. */
    } else loop_start = 0, loop_end = frames - 1u;
    uint32_t bytes; uint8_t format;
    if (looping ? afx_c_encode_sample(resampled, frames, 1, AFX_PCM16, &encoded, &bytes, &format) :
                  afx_c_encode_sample_auto(resampled, frames, 24.0, -INFINITY, AFX_PCM16, &encoded, &bytes, &format)) goto failed;
    free(source); free(resampled);
    (*items)[*count] = (cached_sample_t){wavetable, {encoded, bytes, frames, format, 60, (uint8_t)looping,
                                        (uint16_t)loop_start, (uint16_t)loop_end, 0, rate}, encoded};
    *out = &(*items)[(*count)++].sample; return 0;
failed:
    free(source); free(resampled); free(encoded); return -1;
}

static const double ar_time_ms[64] = {100000,100000,8100,6900,6000,4800,4000,3400,3000,2400,2000,1700,1500,1200,1000,860,760,600,500,430,380,300,250,220,190,150,130,110,95,76,63,55,47,38,31,27,24,19,15,13,12,9.4,7.9,6.8,6,4.7,3.8,3.4,3,2.4,2,1.8,1.6,1.3,1.1,.93,.85,.65,.53,.44,.4,.35,0,0};
static const double dr_time_ms[64] = {100000,100000,118200,101300,88600,70900,59100,50700,44300,35500,29600,25300,22200,17700,14800,12700,11100,8900,7400,6300,5500,4400,3700,3200,2800,2200,1800,1600,1400,1100,920,790,690,550,460,390,340,270,230,200,170,140,110,98,85,68,57,49,43,34,28,25,22,18,14,12,11,8.5,7.1,6.1,5.4,4.3,3.6,3.1};

static int envelope_rate(int32_t microseconds, const double table[64]) {
    if (microseconds <= 0) return 30;
    double target = microseconds / 1000.0; int best = 1;
    for (int rate = 2; rate < 31; ++rate)
        if (fabs(log(table[2 * rate] / target)) < fabs(log(table[2 * best] / target))) best = rate;
    return best;
}

static uint16_t mix_word(const afx_c_note_t *note, uint8_t instrument_volume, uint8_t sample_volume,
                         uint8_t attack_volume, uint8_t pan) {
    unsigned volume = (note->controller_state >> 4) & 127u, expression = (note->controller_state >> 11) & 127u;
    double attenuation = -2000.0 * log10((double)(instrument_volume ? instrument_volume : 1u) *
                                         (sample_volume ? sample_volume : 1u) * (attack_volume ? attack_volume : 1u) /
                                         (127.0 * 127.0 * 127.0));
    attenuation += -300.0 * log10((double)note->velocity / 127.0);
    if (!volume || !expression) attenuation = 10200.0;
    else attenuation += -2000.0 * log10((double)volume * expression / (127.0 * 127.0));
    double pan_cents = ((int)pan - 64) * 500.0 / 63.0;
    if (pan_cents < -500) pan_cents = -500; if (pan_cents > 500) pan_cents = 500;
    double position = (pan_cents + 500) * acos(-1.0) / 2000.0;
    attenuation += -2000.0 * log10(fmax(cos(position), sin(position)) * sqrt(2.0));
    int tl = (int)lround(attenuation / 40.0); if (tl < 0) tl = 0; if (tl > 255) tl = 255;
    return (uint16_t)(tl << 8 | 0x24);
}

static uint16_t direct_word(uint8_t pan) {
    double pan_cents = ((int)pan - 64) * 500.0 / 63.0;
    if (pan_cents < -500) pan_cents = -500; if (pan_cents > 500) pan_cents = 500;
    double angle = (pan_cents + 500) * acos(-1.0) / 2000.0, loud = fmax(cos(angle), sin(angle)), soft = fmin(cos(angle), sin(angle));
    int dipan = (int)lround(-20.0 * log10(fmax(soft / loud, 1e-9)) / (10.0 * log10(2.0)));
    if (dipan > 15) dipan = 15;
    return (uint16_t)(0x0f00 | dipan | (pan_cents <= 0 ? 0x10 : 0));
}

static int append_zone(afx_c_zone_t **zones, uint32_t *count, uint32_t *capacity, afx_c_zone_t zone) {
    if (*count == *capacity) {
        uint32_t next = *capacity ? *capacity * 2u : 128u;
        afx_c_zone_t *grown = realloc(*zones, (size_t)next * sizeof(**zones));
        if (!grown) return -1;
        *zones = grown; *capacity = next;
    }
    (*zones)[(*count)++] = zone; return 0;
}

static int lower(const bank_t *bank, afx_c_note_t *input, uint32_t input_count,
                 afx_c_note_t **out_notes, uint32_t *out_count, afx_c_zone_t **out_zones, uint32_t *out_zone_count) {
    afx_c_note_t *notes = calloc(input_count, sizeof(*notes)); afx_c_zone_t *zones = NULL;
    cached_sample_t *cache = NULL; uint32_t note_count = 0, zone_count = 0, zone_capacity = 0, cache_count = 0, cache_capacity = 0;
    if (!notes) goto failed;
    for (uint32_t index = 0; index < input_count; ++index) {
        afx_c_note_t note = input[index]; uint32_t instrument; sound_t sound; uint8_t ivolume, ipan;
        if (bank_instrument(bank, note.program, (note.controller_state & 15u) == 9u, &instrument)) goto failed;
        int selected = select_sound(bank, instrument, note.key, note.velocity, &sound, &ivolume, &ipan);
        if (selected > 0) continue;
        if (selected < 0) goto failed;
        const afx_c_sample_t *sample;
        if (cache_sample(bank, sound.wavetable, &cache, &cache_count, &cache_capacity, &sample)) goto failed;
        const uint8_t *keymap = bank->control + sound.keymap, *env = bank->control + sound.envelope;
        uint8_t attack_volume = env[12] ? env[12] : 1, decay_volume = env[13] ? env[13] : 1;
        int decay_level = (int)lround(-20.0 * log10((double)decay_volume / attack_volume) / 3.0103);
        if (decay_level < 0) decay_level = 0; if (decay_level > 31) decay_level = 31;
        int channel_pan = (int)((note.controller_state >> 18) & 127u) + ipan - 64 + sound.pan - 64;
        if (channel_pan < 0) channel_pan = 0; if (channel_pan > 127) channel_pan = 127;
        afx_c_zone_t zone = {.sample = *sample, .key_min = keymap[2], .key_max = keymap[3],
            .velocity_min = keymap[0], .velocity_max = keymap[1], .bank_msb = note.bank_msb,
            .bank_lsb = note.bank_lsb, .program = note.program,
            .dsp_send = (uint8_t)(lround(note.controllers[91] * 15.0 / 127.0) << 4),
            .setup_mask = (1u << AFX_FIELD_ENV_AD) | (1u << AFX_FIELD_ENV_DR) | (1u << AFX_FIELD_DIRECT)};
        zone.sample.root_key = keymap[4]; zone.sample.tuning_cents = (int8_t)keymap[5];
        zone.setup[AFX_FIELD_ENV_AD] = (uint16_t)(envelope_rate(sbe32(env), ar_time_ms) |
                                                   envelope_rate(sbe32(env + 4u), dr_time_ms) << 6);
        zone.setup[AFX_FIELD_ENV_DR] = (uint16_t)(envelope_rate(sbe32(env + 8u), dr_time_ms) |
                                                   decay_level << 5 | 15 << 10);
        zone.setup[AFX_FIELD_DIRECT] = direct_word((uint8_t)channel_pan);
        note.mix = mix_word(&note, ivolume, sound.volume, attack_volume, (uint8_t)channel_pan);
        if (append_zone(&zones, &zone_count, &zone_capacity, zone)) goto failed;
        note.setup_index = (uint16_t)zone_count; notes[note_count++] = note;
    }
    if (!note_count) goto failed;
    *out_notes = notes; *out_count = note_count; *out_zones = zones; *out_zone_count = zone_count;
    free(cache); return 0;
failed:
    if (cache) for (uint32_t i = 0; i < cache_count; ++i) free(cache[i].data);
    free(cache); free(notes); free(zones); return -1;
}

int main(int argc, char **argv) {
    if (argc < 6 || argc > 7) { fprintf(stderr, "usage: afx_n64 CONTROL.b1 TABLE.tbl SEQUENCES.s1 INDEX OUTPUT.afx [BANK_INDEX]\n"); return 2; }
    char *end = NULL; long index = strtol(argv[4], &end, 10);
    if (*end || index < 0) return 2;
    long bank_index = 0;
    if (argc == 7) { bank_index = strtol(argv[6], &end, 10); if (*end || bank_index < 0) return 2; }
    uint8_t *control = NULL, *table = NULL, *sequence = NULL; uint32_t control_bytes, table_bytes, sequence_bytes, raw_count, duration, note_count, zone_count;
    afx_c_note_t *raw = NULL, *notes = NULL; afx_c_zone_t *zones = NULL; afx_c_output_t output = {0}; bank_t bank; int status = 1;
    char *afb = NULL, *afc = NULL, *afv = NULL;
    if (read_file(argv[1], &control, &control_bytes) || read_file(argv[2], &table, &table_bytes) ||
        read_file(argv[3], &sequence, &sequence_bytes) || bank_open(&bank, control, control_bytes, table, table_bytes, (uint32_t)bank_index) ||
        afx_c_n64_cseq_notes(sequence, sequence_bytes, (int)index, 1000, &raw, &raw_count, &duration) ||
        lower(&bank, raw, raw_count, &notes, &note_count, &zones, &zone_count) ||
        afx_c_compile_zones(notes, note_count, 1000, zones, zone_count, &output)) {
        fprintf(stderr, "afx_n64: invalid source or AICA limits exceeded\n"); goto done;
    }
    afb = with_suffix(argv[5], ".afb"); afc = with_suffix(argv[5], ".afc"); afv = with_suffix(argv[5], ".afv");
    if (!afb || !afc || !afv || write_file(argv[5], output.afx, output.afx_bytes) || write_file(afb, output.afb, output.afb_bytes) ||
        write_file(afc, output.afc, output.afc_bytes) || write_file(afv, output.afv, output.afv_bytes)) goto done;
    printf("wrote %s (%u notes, %u AFX, %u AFB, %u AFC, %u AFV)\n", argv[5], note_count, output.afx_bytes, output.afb_bytes, output.afc_bytes, output.afv_bytes);
    status = 0;
done:
    /* Zones only borrow cached sample data; free unique cache payloads once. */
    if (zones) for (uint32_t i = 0; i < zone_count; ++i) {
        int first = 1; for (uint32_t j = 0; j < i; ++j) if (zones[j].sample.data == zones[i].sample.data) { first = 0; break; }
        if (first) free((void *)zones[i].sample.data);
    }
    free(afb); free(afc); free(afv); afx_c_output_free(&output); free(control); free(table); free(sequence); free(raw); free(notes); free(zones);
    return status;
}
