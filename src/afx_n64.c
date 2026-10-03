/* Direct N64 B1/S1 -> AFB/AFX/AFC/AFV authoring tool.
 *
 * It deliberately stops at the common authoring IR: B1 lowers to explicit
 * samples/zones and CSeq lowers to notes.  The shared optimizer and emitter
 * own every final AFB/AFX layout decision.
 */
#include "afx_compile_c.h"
#include "afx_n64_cseq.h"
#include "afx_sample_c.h"

#include <aicaflow/codec.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int n64_vadpcm_decode(const uint8_t *data, int data_len, const int16_t *coef,
                      int predictors, int16_t *out);

typedef struct { const uint8_t *control, *table; uint32_t control_bytes, table_bytes, bank, rate, percussion; int instruments; } bank_t;
typedef struct { uint32_t wavetable; afx_c_sample_t sample; uint8_t *data; } cached_sample_t;
typedef struct { uint32_t envelope, keymap, wavetable; uint8_t pan, volume; } sound_t;
typedef struct {
    afx_c_note_t note;
    uint8_t source_channel, channel_volume, channel_pan, sample_pan, reverb;
    int16_t bend, bend_range;
    uint32_t source_start, source_release;
} n64_note_t;
typedef struct { uint32_t instrument; uint8_t volume, pan, reverb; int16_t bend, bend_range; } channel_state_t;

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

static int bank_program_instrument(const bank_t *bank, uint8_t program, int percussion, uint32_t *out) {
    uint32_t at = percussion && bank->percussion ? bank->percussion :
                  (program < bank->instruments ? be32(bank->control + bank->bank + 12u + program * 4u) : 0);
    if (!at || !within(at, bank->control_bytes, 16) || sbe16(bank->control + at + 14u) < 1 ||
        sbe16(bank->control + at + 14u) > 1024 ||
        !within(at + 16u, bank->control_bytes, (uint32_t)sbe16(bank->control + at + 14u) * 4u)) return -1;
    *out = at; return 0;
}

static int set_channel_instrument(const bank_t *bank, channel_state_t *state, uint8_t program, int percussion) {
    uint32_t instrument;
    if (bank_program_instrument(bank, program, percussion, &instrument)) return -1;
    state->instrument = instrument;
    state->volume = bank->control[instrument];
    state->pan = bank->control[instrument + 1u];
    state->bend_range = sbe16(bank->control + instrument + 12u);
    return 0;
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

static uint16_t mix_word(uint8_t velocity, uint8_t channel_volume, uint8_t sample_volume,
                         uint8_t attack_volume, uint8_t pan) {
    double attenuation = -2000.0 * log10((double)(channel_volume ? channel_volume : 1u) *
                                         (sample_volume ? sample_volume : 1u) * (attack_volume ? attack_volume : 1u) /
                                         (127.0 * 127.0 * 127.0));
    attenuation += -300.0 * log10((double)velocity / 127.0);
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

static int source_before(uint32_t tick, uint32_t track, uint32_t order, const afx_c_note_t *note) {
    return tick < note->source_tick ||
           (tick == note->source_tick && (track < note->source_track ||
            (track == note->source_track && order < note->source_order)));
}

static int lower(const bank_t *bank, const afx_c_note_t *input, uint32_t input_count,
                 const afx_n64_automation_t *automation, uint32_t automation_count,
                 n64_note_t **out_notes, uint32_t *out_count, afx_c_zone_t **out_zones, uint32_t *out_zone_count) {
    n64_note_t *notes = calloc(input_count, sizeof(*notes)); afx_c_zone_t *zones = NULL;
    cached_sample_t *cache = NULL; uint32_t note_count = 0, zone_count = 0, zone_capacity = 0, cache_count = 0, cache_capacity = 0;
    channel_state_t channels[16] = {{0}};
    uint32_t cursor = 0;
    if (!notes) goto failed;
    for (uint32_t channel = 0; channel < 16; ++channel) {
        if (bank_instrument(bank, 0, channel == 9, &channels[channel].instrument)) goto failed;
        channels[channel].volume = bank->control[channels[channel].instrument];
        channels[channel].pan = bank->control[channels[channel].instrument + 1u];
        channels[channel].bend_range = sbe16(bank->control + channels[channel].instrument + 12u);
    }
    for (uint32_t index = 0; index < input_count; ++index) {
        const afx_c_note_t *source = input + index;
        uint8_t channel = (uint8_t)(source->controller_state & 15u);
        while (cursor < automation_count && source_before(automation[cursor].source_tick,
               automation[cursor].source_track, automation[cursor].source_order, source)) {
            const afx_n64_automation_t *event = automation + cursor++;
            channel_state_t *state = channels + event->channel;
            if (event->kind == AFX_N64_EVENT_PROGRAM) {
                /* libultra uses percussion only for channel 9's initial
                 * state; an explicit program change always selects instArray. */
                if (set_channel_instrument(bank, state, event->value, 0)) goto failed;
            } else if (event->kind == AFX_N64_EVENT_CONTROL) {
                if (event->control == 7) state->volume = event->value;
                else if (event->control == 10) state->pan = event->value;
                else if (event->control == 91) state->reverb = event->value;
            } else if (event->kind == AFX_N64_EVENT_PITCH) state->bend = event->pitch_bend;
        }
        channel_state_t *state = channels + channel;
        afx_c_note_t note = *source; sound_t sound; uint8_t ignored_volume, ignored_pan;
        int selected = select_sound(bank, state->instrument, note.key, note.velocity, &sound, &ignored_volume, &ignored_pan);
        if (selected > 0) continue;
        if (selected < 0) goto failed;
        const afx_c_sample_t *sample;
        if (cache_sample(bank, sound.wavetable, &cache, &cache_count, &cache_capacity, &sample)) goto failed;
        const uint8_t *keymap = bank->control + sound.keymap, *env = bank->control + sound.envelope;
        uint8_t attack_volume = env[12] ? env[12] : 1, decay_volume = env[13] ? env[13] : 1;
        int decay_level = (int)lround(-20.0 * log10((double)decay_volume / attack_volume) / 3.0103);
        if (decay_level < 0) decay_level = 0; if (decay_level > 31) decay_level = 31;
        int channel_pan = (int)state->pan + sound.pan - 64;
        if (channel_pan < 0) channel_pan = 0; if (channel_pan > 127) channel_pan = 127;
        afx_c_zone_t zone = {.sample = *sample, .key_min = keymap[2], .key_max = keymap[3],
            .velocity_min = keymap[0], .velocity_max = keymap[1], .bank_msb = note.bank_msb,
            .bank_lsb = note.bank_lsb, .program = note.program,
            .dsp_send = (uint8_t)(lround(state->reverb * 15.0 / 127.0) << 4),
            .setup_mask = (1u << AFX_FIELD_ENV_AD) | (1u << AFX_FIELD_ENV_DR) | (1u << AFX_FIELD_DIRECT)};
        zone.sample.root_key = keymap[4]; zone.sample.tuning_cents = (int8_t)keymap[5];
        zone.setup[AFX_FIELD_ENV_AD] = (uint16_t)(envelope_rate(sbe32(env), ar_time_ms) |
                                                   envelope_rate(sbe32(env + 4u), dr_time_ms) << 6);
        zone.setup[AFX_FIELD_ENV_DR] = (uint16_t)(envelope_rate(sbe32(env + 8u), dr_time_ms) |
                                                   decay_level << 5 | 15 << 10);
        zone.setup[AFX_FIELD_DIRECT] = direct_word((uint8_t)channel_pan);
        if (append_zone(&zones, &zone_count, &zone_capacity, zone)) goto failed;
        note.setup_index = (uint16_t)zone_count;
        note.source_id = note_count;
        note.mix = mix_word(note.velocity, state->volume, sound.volume, attack_volume, (uint8_t)channel_pan);
        notes[note_count++] = (n64_note_t){note, channel, state->volume, state->pan, sound.pan,
                                           state->reverb, state->bend, state->bend_range,
                                           note.start_tick, note.release_tick ? note.release_tick : note.end_tick};
    }
    if (!note_count) goto failed;
    *out_notes = notes; *out_count = note_count; *out_zones = zones; *out_zone_count = zone_count;
    free(cache); return 0;
failed:
    if (cache) for (uint32_t i = 0; i < cache_count; ++i) free(cache[i].data);
    free(cache); free(notes); free(zones); return -1;
}

static uint16_t n64_pitch(const n64_note_t *note, const afx_c_zone_t *zone, int16_t bend) {
    uint32_t rate = zone->sample.sample_rate ? zone->sample.sample_rate : 44100;
    int cents = zone->sample.tuning_cents + (int)lround((double)bend * note->bend_range / 8192.0);
    cents += (int)lround(1200.0 * log2((double)rate / 44100.0));
    double ratio = pow(2.0, (((int)note->note.key - zone->sample.root_key) * 100.0 + cents) / 1200.0);
    int octave = (int)floor(log2(ratio));
    int fraction = (int)(1024.0 * (ratio / pow(2.0, octave) - 1.0));
    if (octave < -8) octave = -8; if (octave > 7) octave = 7;
    if (fraction < 0) fraction = 0; if (fraction > 1023) fraction = 1023;
    return (uint16_t)(((octave & 15) << 11) | fraction);
}

static uint16_t mix_for_volume(const n64_note_t *note, uint8_t volume) {
    uint16_t old = note->note.mix;
    if (!volume) return 0xff24;
    double change = -2000.0 * log10((double)volume / (note->channel_volume ? note->channel_volume : 1u));
    int total = (old >> 8) + (int)lround(change / 40.0);
    if (total < 0) total = 0; if (total > 255) total = 255;
    return (uint16_t)(total << 8 | 0x24u);
}

static int append_event(afx_c_event_t **events, uint32_t *count, uint32_t *capacity, afx_c_event_t event) {
    if (*count == *capacity) {
        uint32_t next = *capacity ? *capacity * 2u : 256u;
        afx_c_event_t *grown = realloc(*events, (size_t)next * sizeof(**events));
        if (!grown) return -1;
        *events = grown; *capacity = next;
    }
    (*events)[(*count)++] = event;
    return 0;
}

static int compile_n64(n64_note_t *notes, uint32_t note_count,
                       const afx_n64_automation_t *automation, uint32_t automation_count,
                       uint32_t duration, afx_c_zone_t *zones, uint32_t zone_count,
                       afx_c_output_t *out) {
    afx_c_note_t *scheduled = NULL;
    uint8_t *channels = NULL;
    afx_c_event_t *events = NULL, *optimized = NULL;
    afx_c_zone_t *templates = NULL;
    uint32_t event_count = 0, event_capacity = 0, channel_count = 0, template_count = 0, order = 0, final_tick = duration;
    if (!notes || !note_count || !zones || !zone_count || !out) return -1;
    scheduled = malloc(note_count * sizeof(*scheduled)); channels = malloc(note_count);
    if (!scheduled || !channels) goto failed;
    for (uint32_t i = 0; i < note_count; ++i) scheduled[i] = notes[i].note;
    if (afx_c_schedule_notes(scheduled, note_count, 4) ||
        afx_c_assign_channels(scheduled, note_count, channels, &channel_count)) goto failed;
    for (uint32_t i = 0; i < note_count; ++i) {
        n64_note_t *note = notes + scheduled[i].source_id;
        note->note = scheduled[i];
        const afx_c_zone_t *zone = zones + note->note.setup_index - 1u;
        afx_c_event_t event = {.tick = note->note.start_tick, .order = order++, .opcode = AFX_OP_NOTE,
            .channel = channels[i], .setup = (uint16_t)(note->note.setup_index - 1u), .mask = AFX_NOTE_PL_MASK};
        event.fields[AFX_FIELD_PITCH] = n64_pitch(note, zone, note->bend);
        event.fields[AFX_FIELD_MIX] = note->note.mix;
        if (append_event(&events, &event_count, &event_capacity, event) ||
            append_event(&events, &event_count, &event_capacity, (afx_c_event_t){
                .tick = note->note.release_tick ? note->note.release_tick : note->note.end_tick,
                .order = order++, .opcode = AFX_OP_KEYOFF, .channel = channels[i]})) goto failed;
        if (note->note.end_tick > final_tick) final_tick = note->note.end_tick;
    }
    for (uint32_t i = 0; i < automation_count; ++i) {
        const afx_n64_automation_t *source = automation + i;
        if (source->kind != AFX_N64_EVENT_PITCH &&
            !(source->kind == AFX_N64_EVENT_CONTROL &&
              (source->control == 7 || source->control == 10 || source->control == 91))) continue;
        for (uint32_t n = 0; n < note_count; ++n) {
            const n64_note_t *note = notes + n;
            if (note->source_channel != source->channel || source->tick < note->source_start ||
                source->tick >= note->source_release) continue;
            uint32_t tick = source->tick > note->note.start_tick ? source->tick : note->note.start_tick;
            uint32_t release = note->note.release_tick ? note->note.release_tick : note->note.end_tick;
            if (tick >= release) continue;
            afx_c_event_t event = {.tick = tick, .order = order++, .opcode = AFX_OP_PATCH,
                .channel = 0};
            /* Find the assigned AICA voice once; source_id is the stable map. */
            for (uint32_t j = 0; j < note_count; ++j)
                if (scheduled[j].source_id == note->note.source_id) { event.channel = channels[j]; break; }
            if (source->kind == AFX_N64_EVENT_PITCH) {
                event.mask = 1u << AFX_FIELD_PITCH;
                event.fields[AFX_FIELD_PITCH] = n64_pitch(note, zones + note->note.setup_index - 1u, source->pitch_bend);
            } else if (source->control == 7) {
                event.mask = 1u << AFX_FIELD_MIX;
                event.fields[AFX_FIELD_MIX] = mix_for_volume(note, source->value);
            } else if (source->control == 10) {
                int pan = (int)source->value + note->sample_pan - 64;
                if (pan < 0) pan = 0; if (pan > 127) pan = 127;
                event.mask = 1u << AFX_FIELD_DIRECT;
                event.fields[AFX_FIELD_DIRECT] = direct_word((uint8_t)pan);
            } else {
                event.mask = 1u << AFX_FIELD_DSP_SEND;
                event.fields[AFX_FIELD_DSP_SEND] = (uint16_t)(lround(source->value * 15.0 / 127.0) << 4);
            }
            if (append_event(&events, &event_count, &event_capacity, event)) goto failed;
        }
    }
    if (afx_c_optimize_events(events, event_count, zones, zone_count, &optimized, &templates, &template_count) ||
        afx_c_compile_events(optimized, event_count, final_tick, 1000, templates, template_count, out)) goto failed;
    free(scheduled); free(channels); free(events); free(optimized); free(templates); return 0;
failed:
    free(scheduled); free(channels); free(events); free(optimized); free(templates); return -1;
}

/* Sound effects use the same ALBank samples as CSeq music, but an ALInstrument
 * sound list is a little sequencer of its own: the high key-map bits and the
 * velocity minimum select the next component.  Keep this lowering here so
 * DKR never has a second, Python-only AICA authoring path. */
static int read_sound(const bank_t *bank, uint32_t at, sound_t *out) {
    const uint8_t *control = bank->control;
    uint32_t envelope, keymap, wavetable;
    if (!within(at, bank->control_bytes, 16)) return -1;
    envelope = be32(control + at); keymap = be32(control + at + 4u); wavetable = be32(control + at + 8u);
    if (!within(envelope, bank->control_bytes, 16) || !within(keymap, bank->control_bytes, 6) ||
        !within(wavetable, bank->control_bytes, 20)) return -1;
    *out = (sound_t){envelope, keymap, wavetable, control[at + 12u], control[at + 13u]};
    return 0;
}

static uint16_t sfx_pitch(uint32_t rate, int cents) {
    int rate_cents = (int)lround(1200.0 * log2((double)rate / 44100.0));
    double ratio = pow(2.0, ((double)cents + rate_cents) / 1200.0);
    int octave = (int)floor(log2(ratio));
    int fraction = (int)(1024.0 * (ratio / pow(2.0, octave) - 1.0));
    if (octave < -8) octave = -8; if (octave > 7) octave = 7;
    if (fraction < 0) fraction = 0; if (fraction > 1023) fraction = 1023;
    return (uint16_t)(((octave & 15) << 11) | fraction);
}

static uint16_t sfx_mix(uint8_t sample_volume, uint8_t attack_volume) {
    double gain = (double)(sample_volume ? sample_volume : 1u) * (attack_volume ? attack_volume : 1u) /
                  (127.0 * 127.0);
    int total_level = (int)lround(fmax(0.0, -2000.0 * log10(gain)) / 40.0);
    if (total_level > 255) total_level = 255;
    return (uint16_t)(total_level << 8 | 0x24u);
}

static int lower_sfx(const bank_t *bank, unsigned requested, afx_c_event_t **out_events,
                     uint32_t *out_event_count, uint32_t *out_duration,
                     afx_c_zone_t **out_zones, uint32_t *out_zone_count,
                     int *out_park) {
    uint32_t instrument, sound_count, sound_at, cursor_us = 0, duration = 0, cache_count = 0, cache_capacity = 0;
    cached_sample_t *cache = NULL;
    afx_c_event_t *events = NULL;
    afx_c_zone_t *zones = NULL;
    uint8_t *seen = NULL;
    int status = -1;
    if (!requested || bank_instrument(bank, 0, 0, &instrument)) goto done;
    sound_count = (uint32_t)sbe16(bank->control + instrument + 14u);
    if (requested > sound_count) goto done;
    events = calloc(32, sizeof(*events)); zones = calloc(16, sizeof(*zones)); seen = calloc(sound_count + 1u, 1);
    if (!events || !zones || !seen) goto done;
    unsigned component = requested;
    for (uint32_t index = 0; component; ++index) {
        sound_t sound; const afx_c_sample_t *sample; const uint8_t *keymap, *env;
        uint32_t next, start, keyoff, release, sample_duration; int cents, decay_level, sustain;
        double scale, attack_us, decay_us, release_us, ratio;
        if (index == 16 || component > sound_count || seen[component]) goto done;
        seen[component] = 1;
        sound_at = be32(bank->control + instrument + 16u + (component - 1u) * 4u);
        if (read_sound(bank, sound_at, &sound) || cache_sample(bank, sound.wavetable, &cache, &cache_count, &cache_capacity, &sample)) goto done;
        keymap = bank->control + sound.keymap; env = bank->control + sound.envelope;
        cents = (int)keymap[4] * 100 - 6000;
        if (!(keymap[3] & 0x20u)) cents += (int8_t)keymap[5];
        scale = pow(2.0, (double)cents / 1200.0);
        attack_us = fmax(0.0, (double)sbe32(env)) / scale;
        decay_us = sbe32(env + 4u);
        release_us = fmax(0.0, (double)sbe32(env + 8u)) / scale;
        decay_level = (int)lround(-20.0 * log10((double)(env[13] ? env[13] : 1u) /
                                                 (double)(env[12] ? env[12] : 1u)) / 3.0103);
        if (decay_level < 0) decay_level = 0; if (decay_level > 31) decay_level = 31;
        zones[index] = (afx_c_zone_t){.sample = *sample, .key_max = 127, .velocity_max = 127,
            .dsp_send = (uint8_t)((keymap[3] & 15u) << 4),
            .setup_mask = (1u << AFX_FIELD_ENV_AD) | (1u << AFX_FIELD_ENV_DR) | (1u << AFX_FIELD_DIRECT)};
        zones[index].setup[AFX_FIELD_ENV_AD] = (uint16_t)(envelope_rate((int32_t)lround(attack_us), ar_time_ms) |
            envelope_rate((int32_t)lround(decay_us / scale), dr_time_ms) << 6);
        zones[index].setup[AFX_FIELD_ENV_DR] = (uint16_t)(envelope_rate((int32_t)lround(release_us), dr_time_ms) |
            decay_level << 5 | 15 << 10);
        zones[index].setup[AFX_FIELD_DIRECT] = direct_word(sound.pan);
        start = (uint32_t)lround((double)cursor_us / 1000.0);
        events[index * 2u] = (afx_c_event_t){.tick = start, .order = index * 2u, .opcode = AFX_OP_NOTE,
            .channel = (uint8_t)index, .setup = (uint16_t)index, .mask = AFX_NOTE_PL_MASK};
        events[index * 2u].fields[AFX_FIELD_PITCH] = sfx_pitch(sample->sample_rate, cents);
        events[index * 2u].fields[AFX_FIELD_TOTAL_LEVEL] = sfx_mix(sound.volume, env[12]);
        ratio = pow(2.0, (double)cents / 1200.0);
        sample_duration = (uint32_t)fmax(1.0, lround((double)sample->frames * 1000.0 / (44100.0 * ratio)));
        sustain = sample->loop && decay_us < 0.0;
        if (!sustain) {
            keyoff = start + (uint32_t)fmax(1.0, lround((attack_us + decay_us / scale) / 1000.0));
            release = (uint32_t)fmax(1.0, lround(release_us / 1000.0));
            events[index * 2u + 1u] = (afx_c_event_t){.tick = keyoff, .order = index * 2u + 1u,
                .opcode = AFX_OP_KEYOFF, .channel = (uint8_t)index};
            if (keyoff + release > duration) duration = keyoff + release;
        } else *out_park = 1;
        if (start + sample_duration > duration) duration = start + sample_duration;
        cursor_us += (uint32_t)keymap[1] * 33333u;
        next = (uint32_t)keymap[0] + ((uint32_t)(keymap[2] & 0xc0u) << 2);
        component = next;
        *out_zone_count = index + 1u;
    }
    if (!*out_zone_count) goto done;
    *out_events = events; *out_event_count = *out_zone_count * 2u;
    if (*out_park) {
        uint32_t used = 0;
        for (uint32_t i = 0; i < *out_zone_count; ++i) events[used++] = events[i * 2u];
        *out_event_count = used;
    }
    *out_duration = duration;
    *out_zones = zones; zones = NULL; events = NULL; status = 0;
done:
    /* Zone samples borrow cache data, so transfer that ownership to the caller on success. */
    if (!status) {
        for (uint32_t i = 0; i < *out_zone_count; ++i) {
            for (uint32_t j = 0; j < cache_count; ++j) if ((*out_zones)[i].sample.data == cache[j].data) cache[j].data = NULL;
        }
    }
    if (cache) for (uint32_t i = 0; i < cache_count; ++i) free(cache[i].data);
    free(cache); free(events); free(zones); free(seen); return status;
}

static int write_sfx(const char *control_path, const char *table_path, const char *sound_text, const char *output_path) {
    char *end = NULL; unsigned long requested = strtoul(sound_text, &end, 10);
    uint8_t *control = NULL, *table = NULL; uint32_t control_bytes, table_bytes, events = 0, duration = 0, zones = 0;
    afx_c_event_t *stream = NULL; afx_c_zone_t *setups = NULL; afx_c_output_t output = {0}; bank_t bank; int park = 0, result = 1;
    char *afb = NULL;
    if (!requested || *end || requested > UINT32_MAX || read_file(control_path, &control, &control_bytes) ||
        read_file(table_path, &table, &table_bytes) || bank_open(&bank, control, control_bytes, table, table_bytes, 0) ||
        lower_sfx(&bank, (unsigned)requested, &stream, &events, &duration, &setups, &zones, &park) ||
        afx_c_compile_events(stream, events, duration, 1000, setups, zones, &output)) goto done;
    if (park) {
        uint32_t image = afx_read32(output.afx + 16), stream_at = afx_read32(output.afx + 24), stream_bytes = afx_read32(output.afx + 28);
        if (!stream_bytes || output.afx[image + stream_at + stream_bytes - 1u] != AFX_OP_END) goto done;
        output.afx[image + stream_at + stream_bytes - 1u] = AFX_OP_PARK;
        afx_write32(output.afx + 12, afx_read32(output.afx + 12) | AFX_FLAG_CONTROLLED);
        afx_write32(output.afx + 32, afx_control_id(output.afx + image, afx_read32(output.afx + 20)));
        free(output.afc); output.afc = NULL; output.afc_bytes = 0;
    }
    afb = with_suffix(output_path, ".afb");
    if (!afb || write_file(output_path, output.afx, output.afx_bytes) || write_file(afb, output.afb, output.afb_bytes)) goto done;
    printf("wrote %s (%u components, %s)\n", output_path, zones, park ? "parked" : "one-shot");
    result = 0;
done:
    if (setups) for (uint32_t i = 0; i < zones; ++i) {
        int first = 1; for (uint32_t j = 0; j < i; ++j) if (setups[i].sample.data == setups[j].sample.data) { first = 0; break; }
        if (first) free((void *)setups[i].sample.data);
    }
    free(afb); afx_c_output_free(&output); free(control); free(table); free(stream); free(setups); return result;
}

int main(int argc, char **argv) {
    if (argc == 6 && !strcmp(argv[1], "--sfx"))
        return write_sfx(argv[2], argv[3], argv[4], argv[5]) ?
            (fprintf(stderr, "afx_n64: invalid SFX source or AICA limits exceeded\n"), 1) : 0;
    if (argc < 6 || argc > 7) {
        fprintf(stderr, "usage: afx_n64 CONTROL.b1 TABLE.tbl SEQUENCES.s1 INDEX OUTPUT.afx [BANK_INDEX]\n"
                        "       afx_n64 --sfx CONTROL.b1 TABLE.tbl SOUND_ID OUTPUT.afx\n");
        return 2;
    }
    char *end = NULL; long index = strtol(argv[4], &end, 10);
    if (*end || index < 0) return 2;
    long bank_index = 0;
    if (argc == 7) { bank_index = strtol(argv[6], &end, 10); if (*end || bank_index < 0) return 2; }
    uint8_t *control = NULL, *table = NULL, *sequence = NULL; uint32_t control_bytes, table_bytes, sequence_bytes, raw_count, automation_count, duration, note_count, zone_count;
    afx_c_note_t *raw = NULL; afx_n64_automation_t *automation = NULL; n64_note_t *notes = NULL;
    afx_c_zone_t *zones = NULL; afx_c_output_t output = {0}; bank_t bank; int status = 1;
    char *afb = NULL, *afc = NULL, *afv = NULL;
    if (read_file(argv[1], &control, &control_bytes) || read_file(argv[2], &table, &table_bytes) ||
        read_file(argv[3], &sequence, &sequence_bytes) || bank_open(&bank, control, control_bytes, table, table_bytes, (uint32_t)bank_index) ||
        afx_c_n64_cseq_notes(sequence, sequence_bytes, (int)index, 1000, &raw, &raw_count,
                             &automation, &automation_count, &duration) ||
        lower(&bank, raw, raw_count, automation, automation_count, &notes, &note_count, &zones, &zone_count) ||
        compile_n64(notes, note_count, automation, automation_count, duration, zones, zone_count, &output)) {
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
    free(afb); free(afc); free(afv); afx_c_output_free(&output); free(control); free(table); free(sequence); free(raw); free(automation); free(notes); free(zones);
    return status;
}
