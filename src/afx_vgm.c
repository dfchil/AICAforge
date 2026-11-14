/* Offline MultiPCM VGM/VGZ -> AFB/AFX/AFC/AFV importer.
 *
 * This is the C counterpart of the retired research importer. It deliberately
 * accepts only the VGM subset emitted for Sega MultiPCM captures: ROM blocks,
 * C3/B5 bank and register writes, and the ordinary VGM wait commands. */
#include "afx_compile_c.h"
#include "afx_sample_c.h"

#include <aicaflow/codec.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

enum { TICK_RATE = 1000, VGM_RATE = 44100, MULTIPCM_ROM = 0x89, SLOTS = 28, CHIPS = 2 };

typedef struct { uint8_t *data; uint32_t bytes, frames; uint16_t loop; } sample_t;
typedef struct { uint8_t kind, chip, slot; uint32_t tick, order, mask; uint16_t setup; uint16_t fields[AFX_FIELD_COUNT]; } raw_event_t;
typedef struct { double value, target; uint32_t tick; uint16_t mix; uint8_t interpolate; } level_t;
typedef struct {
    uint8_t registers[SLOTS][11], active[SLOTS], selected, address, banking, has_rom;
    uint32_t banks[2], rom_bytes;
    uint8_t *rom;
    level_t levels[SLOTS];
    double pitch_ratio;
} chip_t;
typedef struct {
    chip_t chips[CHIPS]; uint32_t chip_count, elapsed, order;
    uint32_t total_samples, loop_samples; /* VGM header: the recording's length and its loop's length. */
    raw_event_t *events; uint32_t event_count, event_capacity;
    sample_t *samples; uint32_t sample_count, sample_capacity;
    double gain_db;
} parser_t;

static const uint8_t lfo_rates[8] = {0,14,17,18,19,20,20,21};
static const uint8_t lfo_depths[8] = {0,0,1,1,1,2,3,4};
static const double envelope_ms[64] = {0,0,0,0,6222.95,4978.37,4148.66,3556.01,
    3111.47,2489.21,2074.33,1778,1555.74,1244.63,1037.19,889.02,777.87,622.31,
    518.59,444.54,388.93,311.16,259.32,222.27,194.47,155.60,129.66,111.16,97.23,
    77.82,64.85,55.60,48.62,38.91,32.43,27.80,24.31,19.46,16.24,13.92,12.15,9.75,
    8.12,6.98,6.08,4.90,4.08,3.49,3.04,2.49,2.13,1.90,1.72,1.41,1.18,1.04,.91,.73,
    .59,.50,.45,.45,.45,.45};
static const double ar_ms[64] = {100000,100000,8100,6900,6000,4800,4000,3400,3000,
    2400,2000,1700,1500,1200,1000,860,760,600,500,430,380,300,250,220,190,150,130,
    110,95,76,63,55,47,38,31,27,24,19,15,13,12,9.4,7.9,6.8,6,4.7,3.8,3.4,3,2.4,2,
    1.8,1.6,1.3,1.1,.93,.85,.65,.53,.44,.4,.35,0,0};
static const double dr_ms[64] = {100000,100000,118200,101300,88600,70900,59100,50700,
    44300,35500,29600,25300,22200,17700,14800,12700,11100,8900,7400,6300,5500,4400,
    3700,3200,2800,2200,1800,1600,1400,1100,920,790,690,550,460,390,340,270,230,200,
    170,140,110,98,85,68,57,49,43,34,28,25,22,18,14,12,11,8.5,7.1,6.1,5.4,4.3,3.6,3.1};

static uint32_t le32(const uint8_t *p) { return afx_read32(p); }
static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)p[0] << 8 | p[1]; }
static uint32_t tick(uint32_t samples) { return (uint32_t)(((uint64_t)samples * TICK_RATE + VGM_RATE / 2u) / VGM_RATE); }

static int grow(void **data, uint32_t *capacity, uint32_t count, size_t element) {
    if (count < *capacity) return 0;
    uint32_t next = *capacity ? *capacity * 2u : 64u;
    if (next <= count) next = count + 1u;
    if ((uint64_t)next * element > SIZE_MAX) return -1;
    void *grown = realloc(*data, (size_t)next * element);
    if (!grown) return -1;
    *data = grown; *capacity = next; return 0;
}

static int add_event(parser_t *parser, uint32_t when, uint8_t kind, uint8_t chip, uint8_t slot,
                     uint32_t mask, const uint16_t fields[AFX_FIELD_COUNT], uint16_t setup) {
    if (grow((void **)&parser->events, &parser->event_capacity, parser->event_count, sizeof(*parser->events))) return -1;
    raw_event_t *event = &parser->events[parser->event_count++];
    *event = (raw_event_t){.kind = kind, .chip = chip, .slot = slot, .tick = when,
                           .order = parser->order++, .mask = mask, .setup = setup};
    if (fields) memcpy(event->fields, fields, sizeof(event->fields));
    return 0;
}

static int source_channel(uint8_t value, uint8_t *out) {
    value &= 31u;
    if ((value & 7u) == 7u) return -1;
    *out = (uint8_t)(value - value / 8u); return 0;
}

static uint16_t source_pitch(const uint8_t r[11], double ratio) {
    int octave = ((r[3] >> 4) - 1) & 15;
    if (octave & 8) octave -= 16;
    int fns = (r[3] & 15) * 64 + (r[2] >> 2);
    double value = ldexp((1024.0 + fns) / 1024.0, octave) * ratio;
    octave = (int)floor(log2(value));
    int fraction = (int)nearbyint(1024.0 * (value / exp2(octave) - 1.0));
    if (octave < -8) octave = -8; else if (octave > 7) octave = 7;
    if (fraction < 0) fraction = 0; else if (fraction > 1023) fraction = 1023;
    return (uint16_t)((octave & 15) << 11 | fraction);
}

static uint16_t source_pan(const uint8_t r[11]) {
    uint8_t pan = r[0] >> 4;
    if (pan == 8) return 0;
    if (pan < 7) return (uint16_t)(0x0f00 | pan);
    if (pan == 7) return 0x0f0f;
    if (pan < 15) return (uint16_t)(0x0f10 | (pan - 8));
    return 0x0f1f;
}

static uint16_t source_lfo(const uint8_t r[11]) {
    return (uint16_t)(lfo_rates[(r[6] >> 3) & 7] << 10 | 2 << 8 |
                      lfo_depths[r[6] & 7] << 5 | 2 << 3 | (r[7] & 7));
}

static int envelope_index(const uint8_t r[11], int control) {
    if (!control) return -1;
    if (control == 15) return 63;
    int octave = r[3] >> 4;
    if (octave & 8) octave -= 16;
    int key_rate = r[10] >> 4;
    int offset = key_rate == 15 ? 0 : (octave + key_rate) * 2 + ((r[3] & 8) >> 3);
    int result = control * 4 + offset;
    return result < 0 ? 0 : result > 63 ? 63 : result;
}

static int nearest_rate(double target, const double table[64]) {
    if (target <= 0) return 0;
    int best = 1;
    for (int rate = 2; rate < 31; ++rate)
        if (fabs(log(table[rate * 2] / target)) < fabs(log(table[best * 2] / target))) best = rate;
    return best;
}

static void source_envelope(const uint8_t r[11], uint16_t *ad, uint16_t *dr) {
    int attack = envelope_index(r, r[8] >> 4), decay1 = envelope_index(r, r[8] & 15);
    int decay2 = envelope_index(r, r[9] & 15), release = envelope_index(r, r[10] & 15);
    int ar = attack < 0 ? 0 : nearest_rate(envelope_ms[attack], ar_ms);
    int d1 = decay1 < 0 ? 0 : nearest_rate(envelope_ms[decay1] * 14.32833, dr_ms);
    int d2 = decay2 < 0 ? 0 : nearest_rate(envelope_ms[decay2] * 14.32833, dr_ms);
    int rr = release < 0 ? 0 : nearest_rate(envelope_ms[release] * 14.32833, dr_ms);
    *ad = (uint16_t)(ar | d1 << 6 | d2 << 11);
    *dr = (uint16_t)(rr | (int)nearbyint((r[9] >> 4) * 31.0 / 15.0) << 5 | 15 << 10);
}

static uint16_t source_mix(const uint8_t r[11], double gain_db, double level) {
    int muted = r[0] >> 4 == 8;
    double attenuation = fmax(0.0, 1200.0 - gain_db * 100.0 + level * 37.5);
    int tl = muted ? 255 : (int)nearbyint(attenuation / 40.0);
    if (tl > 255) tl = 255;
    return (uint16_t)(tl << 8 | 0x24);
}

static int sample_for(parser_t *parser, uint8_t chip, uint8_t slot, uint32_t *out) {
    chip_t *state = &parser->chips[chip]; uint8_t *r = state->registers[slot];
    uint32_t ident = r[1] | (uint32_t)(r[2] & 1) << 8, head = ident * 12u;
    if (!state->rom || head > state->rom_bytes || state->rom_bytes - head < 12) return -1;
    const uint8_t *header = state->rom + head;
    uint32_t start = be24(header);
    if (start & 0x400000u) return -1;
    start &= 0x3fffffu;
    if (state->banking && start & 0x100000u)
        start = (start & 0x7ffffu) | state->banks[start & 0x080000u ? 1 : 0];
    uint32_t frames = 0x10000u - be16(header + 5), loop = be16(header + 3);
    if (!frames || frames > 65535 || start > state->rom_bytes || frames > state->rom_bytes - start) return -1;
    if (loop >= frames) loop = frames - 1u;
    if (loop == frames - 1u && loop) --loop;
    const uint8_t *raw = state->rom + start;
    for (uint32_t i = 0; i < parser->sample_count; ++i)
        if (parser->samples[i].frames == frames && parser->samples[i].loop == loop &&
            !memcmp(parser->samples[i].data, raw, frames)) { *out = i; return 0; }
    if (grow((void **)&parser->samples, &parser->sample_capacity, parser->sample_count, sizeof(*parser->samples))) return -1;
    sample_t *sample = &parser->samples[parser->sample_count];
    sample->data = malloc(frames);
    if (!sample->data) return -1;
    memcpy(sample->data, raw, frames); sample->bytes = sample->frames = frames; sample->loop = (uint16_t)loop;
    *out = parser->sample_count++; return 0;
}

static int apply_defaults(chip_t *state, uint8_t slot) {
    uint8_t *r = state->registers[slot]; uint32_t ident = r[1] | (uint32_t)(r[2] & 1) << 8, at = ident * 12u;
    if (!state->rom || at > state->rom_bytes || state->rom_bytes - at < 12) return -1;
    const uint8_t *header = state->rom + at;
    r[6] = header[7]; r[7] = header[11]; memcpy(r + 8, header + 8, 3); return 0;
}

static int add_note(parser_t *parser, uint8_t chip, uint8_t slot) {
    uint32_t sample;
    if (sample_for(parser, chip, slot, &sample)) return -1;
    chip_t *state = &parser->chips[chip]; uint8_t *r = state->registers[slot];
    uint16_t fields[AFX_FIELD_COUNT] = {0};
    source_envelope(r, &fields[AFX_FIELD_ENV_AD], &fields[AFX_FIELD_ENV_DR]);
    fields[AFX_FIELD_PITCH] = source_pitch(r, state->pitch_ratio);
    fields[AFX_FIELD_LFO] = source_lfo(r); fields[AFX_FIELD_DIRECT] = source_pan(r);
    fields[AFX_FIELD_MIX] = source_mix(r, parser->gain_db, state->levels[slot].value);
    state->levels[slot].mix = fields[AFX_FIELD_MIX];
    return add_event(parser, tick(parser->elapsed), AFX_OP_NOTE, chip, slot,
                     AFX_NOTE_PL_MASK | (1u << AFX_FIELD_LFO) | (1u << AFX_FIELD_DIRECT), fields, (uint16_t)sample);
}

static int add_patch(parser_t *parser, uint32_t when, uint8_t chip, uint8_t slot,
                     uint32_t mask, const uint16_t fields[AFX_FIELD_COUNT]) {
    return add_event(parser, when, AFX_OP_PATCH, chip, slot, mask, fields, 0);
}

static int advance_level(parser_t *parser, uint8_t chip, uint8_t slot, uint32_t when) {
    chip_t *state = &parser->chips[chip]; level_t *level = &state->levels[slot];
    uint32_t now = tick(parser->elapsed);
    if (when < level->tick) return -1;
    if (!state->active[slot]) { level->tick = when; return 0; }
    for (uint32_t current = level->tick + 1u; current <= when; ++current) {
        if (level->interpolate && level->value != level->target) {
            double step = level->value > level->target ? 128.0 / 78.2 : 128.0 / 156.4;
            level->value = level->value > level->target ? fmax(level->target, level->value - step) : fmin(level->target, level->value + step);
        }
        uint16_t mix = source_mix(state->registers[slot], parser->gain_db, level->value);
        if (mix != level->mix) {
            uint16_t fields[AFX_FIELD_COUNT] = {0}; fields[AFX_FIELD_MIX] = mix;
            if (add_patch(parser, current, chip, slot, 1u << AFX_FIELD_MIX, fields)) return -1;
            level->mix = mix;
        }
        if (current == UINT32_MAX) break;
    }
    level->tick = now > when ? now : when;
    return 0;
}

static int replace_rom(chip_t *state, uint32_t size, uint32_t start, const uint8_t *data, uint32_t bytes) {
    if (!size || start > size || bytes > size - start) return -1;
    if (!state->rom) { state->rom = malloc(size); if (!state->rom) return -1; memset(state->rom, 0xff, size); state->rom_bytes = size; }
    if (state->rom_bytes != size) return -1;
    memcpy(state->rom + start, data, bytes); state->has_rom |= bytes != 0; return 0;
}

static int write_register(parser_t *parser, uint8_t chip, uint8_t port, uint8_t value) {
    if (chip >= parser->chip_count) return -1;
    chip_t *state = &parser->chips[chip]; uint32_t now = tick(parser->elapsed);
    if (port == 1) return source_channel(value, &state->selected);
    if (port == 2) { state->address = value < 8 ? value : 7; return 0; }
    if (port == 0x10) { state->banking = 1; state->banks[0] = (uint32_t)value << 20; state->banks[1] = state->banks[0] | 0x080000; return 0; }
    if (port == 0x11) { state->banking = 1; state->banks[0] = (uint32_t)value << 19; return 0; }
    if (port == 0x12) { state->banking = 1; state->banks[1] = (uint32_t)value << 19; return 0; }
    if (port != 0) return 0; /* Unsupported ports do not affect the source voice state. */
    uint8_t slot = state->selected, reg = state->address; int active = state->active[slot];
    if (active && advance_level(parser, chip, slot, now)) return -1;
    state->registers[slot][reg] = value;
    if (reg == 1) {
        if (apply_defaults(state, slot)) { if (active) return -1; }
        else if (active) {
            level_t *level = &state->levels[slot]; level->value = level->target; level->tick = now;
            if (add_note(parser, chip, slot)) return -1;
        }
    }
    if (reg == 4) {
        if (value & 0x80) {
            level_t *level = &state->levels[slot]; level->value = level->target; level->tick = now;
            if (add_note(parser, chip, slot)) return state->has_rom ? -1 : 0;
            state->active[slot] = 1;
        } else if (active) {
            if ((state->registers[slot][10] & 15) == 15) {
                uint16_t fields[AFX_FIELD_COUNT] = {0}; fields[AFX_FIELD_MIX] = 0xff24;
                if (add_patch(parser, now ? now - 1u : 0, chip, slot, 1u << AFX_FIELD_MIX, fields)) return -1;
            }
            if (add_event(parser, now, AFX_OP_KEYOFF, chip, slot, 0, NULL, 0)) return -1;
            state->active[slot] = 0;
        }
    } else if (reg == 5) {
        level_t *level = &state->levels[slot]; level->target = value >> 1; level->interpolate = !(value & 1);
        if (!level->interpolate) level->value = level->target;
        if (active) {
            uint16_t mix = source_mix(state->registers[slot], parser->gain_db, level->value);
            if (mix != level->mix) { uint16_t fields[AFX_FIELD_COUNT] = {0}; fields[AFX_FIELD_MIX] = mix;
                if (add_patch(parser, now, chip, slot, 1u << AFX_FIELD_MIX, fields)) return -1;
                level->mix = mix; }
        }
    } else if (active && (reg == 0 || reg == 2 || reg == 3 || reg == 6 || reg == 7)) {
        uint16_t fields[AFX_FIELD_COUNT] = {0}; uint32_t mask;
        if (reg == 0) { fields[AFX_FIELD_DIRECT] = source_pan(state->registers[slot]); fields[AFX_FIELD_MIX] = source_mix(state->registers[slot], parser->gain_db, state->levels[slot].value); mask = (1u << AFX_FIELD_DIRECT) | (1u << AFX_FIELD_MIX); state->levels[slot].mix = fields[AFX_FIELD_MIX]; }
        else if (reg == 2 || reg == 3) { fields[AFX_FIELD_PITCH] = source_pitch(state->registers[slot], state->pitch_ratio); mask = 1u << AFX_FIELD_PITCH; }
        else { fields[AFX_FIELD_LFO] = source_lfo(state->registers[slot]); mask = 1u << AFX_FIELD_LFO; }
        if (add_patch(parser, now, chip, slot, mask, fields)) return -1;
    }
    return 0;
}

static int compare_raw(const void *left, const void *right) {
    const raw_event_t *a = left, *b = right;
    if (a->tick != b->tick) return a->tick < b->tick ? -1 : 1;
    return a->order < b->order ? -1 : a->order > b->order;
}

static int add_zone(afx_c_zone_t **zones, uint32_t *count, uint32_t *capacity, const parser_t *parser,
                    uint16_t sample, uint16_t ad, uint16_t dr, uint16_t direct, uint16_t lfo, uint16_t *out) {
    for (uint32_t i = 0; i < *count; ++i)
        if ((*zones)[i].sample.data == parser->samples[sample].data && (*zones)[i].setup[AFX_FIELD_ENV_AD] == ad &&
            (*zones)[i].setup[AFX_FIELD_ENV_DR] == dr && (*zones)[i].setup[AFX_FIELD_DIRECT] == direct &&
            (*zones)[i].setup[AFX_FIELD_LFO] == lfo) { *out = (uint16_t)i; return 0; }
    if (*count == UINT16_MAX || grow((void **)zones, capacity, *count, sizeof(**zones))) return -1;
    sample_t source = parser->samples[sample];
    (*zones)[*count] = (afx_c_zone_t){.sample = {source.data, source.bytes, source.frames, AFX_PCM8, 60, 1,
        source.loop, source.frames - 1u, 0, 44100}, .key_max = 127, .velocity_max = 127,
        .setup_mask = (1u << AFX_FIELD_ENV_AD) | (1u << AFX_FIELD_ENV_DR) |
                      (1u << AFX_FIELD_DIRECT) | (1u << AFX_FIELD_LFO),
        .setup = {[AFX_FIELD_ENV_AD] = ad, [AFX_FIELD_ENV_DR] = dr,
                  [AFX_FIELD_DIRECT] = direct, [AFX_FIELD_LFO] = lfo}};
    *out = (uint16_t)(*count)++; return 0;
}

static int compact(parser_t *parser, afx_c_event_t **out_events, uint32_t *out_count,
                   afx_c_zone_t **out_zones, uint32_t *out_zone_count) {
    afx_c_event_t *events = NULL; afx_c_zone_t *zones = NULL;
    uint32_t event_capacity = 0, zone_capacity = 0, count = 0, zones_count = 0, ordered = 0;
    qsort(parser->events, parser->event_count, sizeof(*parser->events), compare_raw);
    for (uint32_t at = 0; at < parser->event_count;) {
        uint32_t end = at + 1, when = parser->events[at].tick;
        while (end < parser->event_count && parser->events[end].tick == when) ++end;
        uint32_t patch_mask[CHIPS][SLOTS] = {{0}};
        uint16_t patch_values[CHIPS][SLOTS][AFX_FIELD_COUNT] = {{{0}}}; uint8_t keyoff[CHIPS][SLOTS] = {{0}};
        for (uint32_t i = at; i < end; ++i) {
            raw_event_t *source = &parser->events[i];
            if (source->kind == AFX_OP_PATCH) {
                patch_mask[source->chip][source->slot] |= source->mask;
                for (uint32_t field = 0; field < AFX_FIELD_COUNT; ++field)
                    if (source->mask & (1u << field)) patch_values[source->chip][source->slot][field] = source->fields[field];
            } else if (source->kind == AFX_OP_KEYOFF) keyoff[source->chip][source->slot] = 1;
        }
        const uint8_t kinds[] = {AFX_OP_KEYOFF, AFX_OP_NOTE};
        for (uint32_t kind_index = 0; kind_index < 2; ++kind_index)
            for (uint32_t i = at; i < end; ++i) {
                raw_event_t *source = &parser->events[i]; if (source->kind != kinds[kind_index]) continue;
                if (grow((void **)&events, &event_capacity, count, sizeof(*events))) goto failed;
                afx_c_event_t *target = &events[count++];
                *target = (afx_c_event_t){.tick = when, .order = ordered++, .opcode = source->kind,
                                           .channel = (uint8_t)(source->chip * SLOTS + source->slot)};
                if (source->kind == AFX_OP_NOTE) {
                    if (add_zone(&zones, &zones_count, &zone_capacity, parser, source->setup,
                                 source->fields[AFX_FIELD_ENV_AD], source->fields[AFX_FIELD_ENV_DR],
                                 source->fields[AFX_FIELD_DIRECT], source->fields[AFX_FIELD_LFO], &target->setup)) goto failed;
                    target->mask = AFX_NOTE_PL_MASK; memcpy(target->fields, source->fields, sizeof(target->fields));
                }
            }
        for (uint32_t chip = 0; chip < parser->chip_count; ++chip) for (uint32_t slot = 0; slot < SLOTS; ++slot)
            if (patch_mask[chip][slot] && !keyoff[chip][slot]) {
                if (grow((void **)&events, &event_capacity, count, sizeof(*events))) goto failed;
                events[count++] = (afx_c_event_t){.tick = when, .order = ordered++, .opcode = AFX_OP_PATCH,
                    .channel = (uint8_t)(chip * SLOTS + slot), .mask = patch_mask[chip][slot]};
                memcpy(events[count - 1].fields, patch_values[chip][slot], sizeof(events[count - 1].fields));
            }
        at = end;
    }
    *out_events = events; *out_count = count; *out_zones = zones; *out_zone_count = zones_count; return 0;
failed:
    free(events); free(zones); return -1;
}

static int read_vgm(const char *path, uint8_t **out, uint32_t *out_bytes) {
    gzFile file = gzopen(path, "rb"); uint8_t *data = NULL; uint32_t bytes = 0, capacity = 0;
    if (!file) return -1;
    for (;;) {
        if (capacity - bytes < 65536u) { uint32_t next = capacity ? capacity * 2u : 65536u; uint8_t *grown = realloc(data, next); if (!grown) goto failed; data = grown; capacity = next; }
        int got = gzread(file, data + bytes, capacity - bytes);
        if (got < 0 || (!got && !gzeof(file))) goto failed;
        bytes += (uint32_t)got; if (!got) break;
    }
    gzclose(file); *out = data; *out_bytes = bytes; return 0;
failed:
    gzclose(file); free(data); return -1;
}

static void parser_free(parser_t *parser) {
    for (uint32_t i = 0; i < parser->chip_count; ++i) free(parser->chips[i].rom);
    for (uint32_t i = 0; i < parser->sample_count; ++i) free(parser->samples[i].data);
    free(parser->events); free(parser->samples);
}

static int parse_vgm(const char *path, double gain_db, parser_t *parser) {
    uint8_t *data = NULL; uint32_t bytes = 0, position; int stage = 0;
    stage = 1;
    if (read_vgm(path, &data, &bytes) || bytes < 0x8c || memcmp(data, "Vgm ", 4) || le32(data + 8) < 0x161 || !le32(data + 0x88)) goto failed;
    uint32_t clock = le32(data + 0x88); parser->chip_count = clock & 0x40000000u ? 2 : 1; clock &= 0x3fffffffu;
    if (!clock || (le32(data + 0x34) > UINT32_MAX - 0x34u)) goto failed;
    position = 0x34u + le32(data + 0x34); if (position < 0x40 || position >= bytes) goto failed;
    parser->gain_db = gain_db;
    parser->total_samples = le32(data + 0x18); parser->loop_samples = le32(data + 0x1c) ? le32(data + 0x20) : 0;
    for (uint32_t i = 0; i < parser->chip_count; ++i) parser->chips[i].pitch_ratio = (double)clock / 180.0 / VGM_RATE;
    stage = 2;
    while (position < bytes) {
        uint32_t offset = position; uint8_t op = data[position++];
        stage = 0x100 + op;
        if (op == 0x66) break;
        if (op == 0x61) { if (position > bytes || bytes - position < 2) goto failed; parser->elapsed += afx_read16(data + position); position += 2; }
        else if (op == 0x62) parser->elapsed += 735;
        else if (op == 0x63) parser->elapsed += 882;
        else if (op >= 0x70 && op <= 0x7f) parser->elapsed += op - 0x6f;
        else if (op == 0x67) {
            if (position > bytes || bytes - position < 6 || data[position] != 0x66) goto failed;
            uint8_t kind = data[position + 1]; uint32_t length = le32(data + position + 2), chip = length >> 31; length &= 0x7fffffffu; position += 6;
            if (chip >= parser->chip_count || kind != MULTIPCM_ROM || length < 8 || position > bytes || length > bytes - position) goto failed;
            uint32_t size = le32(data + position), start = le32(data + position + 4);
            if (replace_rom(&parser->chips[chip], size, start, data + position + 8, length - 8)) goto failed;
            position += length;
        } else if (op == 0xc3) {
            if (position > bytes || bytes - position < 3) goto failed;
            uint8_t select = data[position], bank = data[position + 1], chip = select >> 7, sides = select & 3; position += 3;
            if (chip >= parser->chip_count) goto failed;
            chip_t *state = &parser->chips[chip]; state->banking = 1;
            if (sides == 3 && !(bank & 8)) { state->banks[0] = (uint32_t)(bank / 16) << 20; state->banks[1] = state->banks[0] | 0x080000; }
            else { if (sides & 2) state->banks[0] = (uint32_t)(bank / 8) << 19; if (sides & 1) state->banks[1] = (uint32_t)(bank / 8) << 19; }
        } else if (op == 0xb5) {
            if (position > bytes || bytes - position < 2) goto failed;
            uint8_t port = data[position], value = data[position + 1], chip = port >> 7; position += 2;
            if (write_register(parser, chip, port & 0x7f, value)) goto failed;
        } else if (op != 0) { (void)offset; goto failed; }
    }
    if (!parser->event_count || !parser->sample_count) goto failed;
    uint32_t duration = tick(parser->elapsed);
    for (uint32_t chip = 0; chip < parser->chip_count; ++chip) for (uint32_t slot = 0; slot < SLOTS; ++slot)
        if (parser->chips[chip].active[slot] && advance_level(parser, chip, slot, duration)) goto failed;
    free(data); return 0;
failed:
    fprintf(stderr, "afx_vgm: parse failure stage %#x\n", stage);
    free(data); return -1;
}

static int write_file(const char *path, const uint8_t *data, uint32_t bytes) {
    FILE *file = fopen(path, "wb");
    if (!file) return -1;
    int failed = fwrite(data, 1, bytes, file) != bytes || fclose(file);
    return failed ? -1 : 0;
}

static char *with_suffix(const char *path, const char *suffix) {
    const char *dot = strrchr(path, '.'); size_t head = dot ? (size_t)(dot - path) : strlen(path), tail = strlen(suffix);
    char *result = malloc(head + tail + 1); if (!result) return NULL;
    memcpy(result, path, head); memcpy(result + head, suffix, tail + 1); return result;
}

static int encode_adpcm_zones(afx_c_zone_t *zones, uint32_t count, uint8_t ***owned, uint32_t *owned_count) {
    const uint8_t **source = calloc(count, sizeof(*source)); uint8_t **data = calloc(count, sizeof(*data));
    if (!source || !data) { free(source); free(data); return -1; }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t prior = 0;
        while (prior < i && source[prior] != zones[i].sample.data) ++prior;
        if (prior < i) {
            zones[i].sample.data = data[prior]; zones[i].sample.bytes = zones[prior].sample.bytes;
            zones[i].sample.format = AFX_ADPCM; continue;
        }
        uint32_t frames = zones[i].sample.frames; uint8_t *pcm16 = malloc((size_t)frames * 2u), *encoded = NULL;
        uint32_t bytes = 0; uint8_t format = 0;
        if (!pcm16) goto failed;
        for (uint32_t frame = 0; frame < frames; ++frame) { pcm16[2 * frame] = 0; pcm16[2 * frame + 1] = zones[i].sample.data[frame]; }
        if (afx_c_encode_sample(pcm16, frames, 1, AFX_ADPCM, &encoded, &bytes, &format) || format != AFX_ADPCM) {
            free(pcm16); goto failed;
        }
        free(pcm16); source[i] = zones[i].sample.data; data[i] = encoded;
        zones[i].sample.data = encoded; zones[i].sample.bytes = bytes; zones[i].sample.format = AFX_ADPCM;
    }
    free(source); *owned = data; *owned_count = count; return 0;
failed:
    for (uint32_t i = 0; i < count; ++i) free(data[i]);
    free(source); free(data); return -1;
}

/* ---------------------------------------------------------------- songs
 * A recording that plays intro, A, A, B and loops (the VGM loop jumps from the
 * end of B back into A's opening) becomes a song: sections intro, A, B, with
 * A stored once. Where A's second play ends differently, A splits into A
 * (the part both plays share) and A-end (the first play's ending); the second
 * ending stays at the start of B. Every join starts a new play, so a section
 * keeps the arcade's own voice allocation and its voices ring out on their own:
 * a section owns the voices that start in its body, with all their later
 * events (its tail). A voice the source stopped only by taking its channel
 * gets a key-off there, and each channel's last voice is given back (FREE)
 * once its envelope is down 60 dB. */
typedef struct {
    uint32_t start, last, cut, keyoff, first; /* ticks; cut/keyoff UINT32_MAX for none; first event index */
    uint16_t ad, dr, pitch; uint8_t channel;
} voice_t;

static int envelope_rate(uint16_t dr, uint16_t pitch, uint32_t rate) {
    if (!rate) return 0;
    int base = 0, krs = (dr >> 10) & 15;
    if (krs != 15) { int octave = (pitch >> 11) & 15; if (octave & 8) octave -= 16; base = octave + 2 * krs + ((pitch >> 9) & 1); }
    int r = base + 2 * (int)rate; return r < 0 ? 0 : r > 63 ? 63 : r;
}

/* When the voice's envelope is 60 dB down (attenuation 640 of 1023), assuming
 * it was at full level when each stage began; UINT32_MAX when it holds. */
static uint32_t voice_silent(const voice_t *v, uint32_t keyoff) {
    const double quiet = 640.0;
    if (keyoff != UINT32_MAX) {
        int rr = envelope_rate(v->dr, v->pitch, v->dr & 31);
        if (rr < 2) return UINT32_MAX;
        return keyoff + (uint32_t)ceil(dr_ms[rr] * quiet / 1024.0) + 1u;
    }
    int ar = envelope_rate(v->dr, v->pitch, v->ad & 31), d1 = envelope_rate(v->dr, v->pitch, (v->ad >> 6) & 31);
    int d2 = envelope_rate(v->dr, v->pitch, (v->ad >> 11) & 31);
    double level = ((v->dr >> 5) & 31) * 32.0, ms = ar < 2 ? 0 : ar_ms[ar];
    if (level >= quiet) { if (d1 < 2) return UINT32_MAX; ms += dr_ms[d1] * quiet / 1024.0; }
    else {
        if (level > 0) { if (d1 < 2) return UINT32_MAX; ms += dr_ms[d1] * level / 1024.0; }
        if (d2 < 2) return UINT32_MAX;
        ms += dr_ms[d2] * (quiet - level) / 1024.0;
    }
    return v->start + (uint32_t)ceil(ms) + 1u;
}

typedef struct { uint32_t index; uint32_t tick; uint8_t opcode, channel; } song_item_t; /* index UINT32_MAX: made here */

static int compare_items(const void *a, const void *b) {
    const song_item_t *x = a, *y = b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    uint32_t xi = x->index == UINT32_MAX ? UINT32_MAX - 1u : x->index, yi = y->index == UINT32_MAX ? UINT32_MAX - 1u : y->index;
    if (xi != yi) return xi < yi ? -1 : 1;
    return (x->opcode > y->opcode) - (x->opcode < y->opcode); /* KEYOFF before FREE */
}

/* core_start, period, core_end in ticks; core_end == 0 means core_start + period. */
static int compile_vgm_song(const parser_t *parser, const afx_c_event_t *events, uint32_t count,
                            afx_c_zone_t *zones, uint32_t zone_count, uint32_t core_start, uint32_t period,
                            uint32_t core_end, int adpcm, int trim, afx_c_output_t *out) {
    int status = -1; uint8_t *section_of = NULL, **adpcm_data = NULL; uint32_t adpcm_count = 0;
    if (!parser->loop_samples || parser->loop_samples > parser->total_samples) { fprintf(stderr, "afx_vgm: --song needs a VGM loop\n"); return -1; }
    uint32_t end = tick(parser->total_samples), loop = tick(parser->total_samples - parser->loop_samples);
    if (!core_end) core_end = core_start + period;
    uint32_t b_end = end - (loop - core_start);
    if (!core_start || loop < core_start || core_end <= core_start || core_end > core_start + period || core_end + period >= b_end) {
        fprintf(stderr, "afx_vgm: --song %u:%u:%u does not fit the recording (loop %u..%u)\n", core_start, period, core_end, loop, end); return -1;
    }
    voice_t *voices = calloc(count + 1u, sizeof(*voices)); uint32_t *owner = malloc((count + 1u) * sizeof(*owner));
    song_item_t *items = NULL; afx_c_event_t *all = NULL, *optimized = NULL; afx_c_zone_t *templates = NULL; uint32_t template_count = 0;
    uint32_t voice_count = 0, current[64]; for (int c = 0; c < 64; ++c) current[c] = UINT32_MAX;
    if (!voices || !owner) goto done;
    for (uint32_t e = 0; e < count; ++e) {
        const afx_c_event_t *event = &events[e]; uint8_t c = event->channel; owner[e] = UINT32_MAX;
        if (c >= 64) goto done;
        if (event->opcode == AFX_OP_NOTE) {
            if (current[c] != UINT32_MAX) voices[current[c]].cut = event->tick;
            voice_t *v = &voices[voice_count]; current[c] = voice_count++;
            *v = (voice_t){.start = event->tick, .last = event->tick, .cut = UINT32_MAX, .keyoff = UINT32_MAX, .first = e, .channel = c,
                           .ad = event->fields[AFX_FIELD_ENV_AD], .dr = event->fields[AFX_FIELD_ENV_DR], .pitch = event->fields[AFX_FIELD_PITCH]};
        }
        if (current[c] == UINT32_MAX) continue;
        voice_t *v = &voices[current[c]]; owner[e] = current[c]; v->last = event->tick;
        if (event->opcode == AFX_OP_KEYOFF && v->keyoff == UINT32_MAX) v->keyoff = event->tick;
        if (event->opcode == AFX_OP_PATCH) {
            if (event->mask & (1u << AFX_FIELD_ENV_AD)) v->ad = event->fields[AFX_FIELD_ENV_AD];
            if (event->mask & (1u << AFX_FIELD_ENV_DR)) v->dr = event->fields[AFX_FIELD_ENV_DR];
            if (event->mask & (1u << AFX_FIELD_PITCH)) v->pitch = event->fields[AFX_FIELD_PITCH];
        }
    }
    /* Sections: intro, A, A-end (when A's plays end differently), B. B's last
     * stretch repeats A's opening up to the loop point; a note there that is
     * no copy of one in A stays with B. */
    enum { INTRO, CORE, CORE_END, B, SECTIONS };
    uint32_t window[SECTIONS][2] = {{0, core_start}, {core_start, core_end}, {core_end, core_start + period}, {core_end + period, b_end}};
    int has_end = core_end < core_start + period;
    section_of = malloc(voice_count + 1u);
    if (!section_of) goto done;
    for (uint32_t i = 0; i < voice_count; ++i) {
        voice_t *v = &voices[i]; section_of[i] = 0xff;
        for (int s = 0; s < SECTIONS; ++s) if (v->start >= window[s][0] && v->start < window[s][1]) section_of[i] = (uint8_t)s;
        if (v->start >= b_end && v->start < end) {
            const afx_c_event_t *note = &events[v->first]; int copy = 0;
            for (uint32_t j = 0; j < voice_count && !copy; ++j) {
                const voice_t *w = &voices[j]; const afx_c_event_t *other = &events[w->first];
                if (w->start < core_start || w->start >= loop) continue;
                uint32_t at = w->start + (end - loop);
                copy = (at + 15u >= v->start && at <= v->start + 15u && other->setup == note->setup &&
                        !memcmp(other->fields, note->fields, sizeof(note->fields)));
            }
            if (!copy) section_of[i] = B;
        }
    }
    uint32_t capacity = count * 2u + 4096u, n = 0, section_first[SECTIONS + 1], body[SECTIONS], last_tick[SECTIONS], added = 0, freed = 0;
    items = malloc(capacity * sizeof(*items));
    if (!items) goto done;
    for (int s = 0; s < SECTIONS; ++s) {
        section_first[s] = n; body[s] = window[s][1] - window[s][0]; last_tick[s] = body[s];
        if (s == CORE_END && !has_end) continue;
        uint32_t base = window[s][0], last_voice[64]; for (int c = 0; c < 64; ++c) last_voice[c] = UINT32_MAX;
        for (uint32_t e = 0; e < count; ++e)
            if (owner[e] != UINT32_MAX && section_of[owner[e]] == s) {
                if (n + 2u >= capacity) goto done;
                items[n++] = (song_item_t){e, events[e].tick - base, events[e].opcode, events[e].channel};
                if (events[e].opcode == AFX_OP_NOTE) last_voice[events[e].channel] = owner[e];
            }
        for (uint32_t i = 0; i < voice_count; ++i) {
            if (section_of[i] != s) continue;
            voice_t *v = &voices[i];
            /* Taken after the body by a note of another play (or never): key it off there if it still sounds. */
            uint32_t cut = v->cut != UINT32_MAX ? v->cut : end, keyoff = v->keyoff;
            if (keyoff == UINT32_MAX && cut >= window[s][1] && voice_silent(v, UINT32_MAX) > cut) {
                if (n + 2u >= capacity) goto done;
                items[n++] = (song_item_t){UINT32_MAX, cut - base, AFX_OP_KEYOFF, v->channel}; keyoff = cut; ++added;
            }
            if (last_voice[v->channel] == i) {
                uint32_t quiet = voice_silent(v, keyoff);
                if (quiet == UINT32_MAX) { fprintf(stderr, "afx_vgm: a voice on channel %u holds forever\n", v->channel); goto done; }
                if (quiet < v->last + 1u) quiet = v->last + 1u;
                items[n++] = (song_item_t){UINT32_MAX, quiet - base, AFX_OP_FREE, v->channel}; ++freed;
            }
        }
        qsort(items + section_first[s], n - section_first[s], sizeof(*items), compare_items);
        for (uint32_t k = section_first[s]; k < n; ++k) if (items[k].tick + 1u > last_tick[s]) last_tick[s] = items[k].tick + 1u;
    }
    section_first[SECTIONS] = n;
    /* One setup dictionary for the whole song: optimize every section's events together. */
    all = calloc(n + 1u, sizeof(*all));
    if (!all) goto done;
    for (uint32_t k = 0; k < n; ++k) {
        if (items[k].index != UINT32_MAX) all[k] = events[items[k].index];
        else { all[k].opcode = items[k].opcode; all[k].channel = items[k].channel; }
        all[k].tick = items[k].tick; all[k].order = k;
    }
    uint32_t trimmed = 0;
    if (trim) { /* each section on its own channels: what it reads of each sample */
        uint32_t *reach = calloc(zone_count, sizeof(*reach));
        if (!reach) goto done;
        for (int s = 0; s < SECTIONS; ++s)
            if (afx_c_sample_reach(all + section_first[s], section_first[s + 1] - section_first[s], zones, zone_count, TICK_RATE, reach)) { free(reach); goto done; }
        trimmed = afx_c_trim_zones(zones, zone_count, reach); free(reach);
    }
    if (afx_c_optimize_events(all, n, zones, zone_count, &optimized, &templates, &template_count)) { fprintf(stderr, "afx_vgm: cannot optimize song events\n"); goto done; }
    if (adpcm && encode_adpcm_zones(templates, template_count, &adpcm_data, &adpcm_count)) { fprintf(stderr, "afx_vgm: ADPCM encoding failed\n"); goto done; }
    afx_c_section_t sections[SECTIONS]; uint32_t used = 0, map[SECTIONS];
    for (int s = 0; s < SECTIONS; ++s) {
        if (s == CORE_END && !has_end) { map[s] = UINT32_MAX; continue; }
        map[s] = used;
        sections[used++] = (afx_c_section_t){optimized + section_first[s], section_first[s + 1] - section_first[s], body[s], last_tick[s], AFX_JOIN_NEW};
    }
    uint16_t order[5]; uint32_t order_count = 0;
    order[order_count++] = (uint16_t)map[INTRO]; order[order_count++] = (uint16_t)map[CORE];
    if (has_end) order[order_count++] = (uint16_t)map[CORE_END];
    order[order_count++] = (uint16_t)map[CORE]; order[order_count++] = (uint16_t)map[B];
    if (afx_c_compile_song(sections, used, order, order_count, 1, TICK_RATE, templates, template_count, out)) {
        fprintf(stderr, "afx_vgm: AICAflow would refuse this song (sections, body..end ticks:");
        for (uint32_t k = 0; k < used; ++k) fprintf(stderr, " %u..%u", sections[k].body_ticks, sections[k].end_ticks);
        fprintf(stderr, ")\n"); goto done;
    }
    printf("song: intro %u, A %u", body[INTRO], body[CORE]);
    if (has_end) printf(", A-end %u", body[CORE_END]);
    printf(", B %u ticks; loop %u..%u; tails", body[B], loop, end);
    for (int s = 0; s < SECTIONS; ++s) if (map[s] != UINT32_MAX) printf(" %u", last_tick[s] - body[s]);
    printf(" ticks; %u key-offs added, %u FREE; %u events moved by %u ticks", added, freed, out->moved_events, out->moved_ticks);
    if (trim) printf("; %u sample frames never read, trimmed", trimmed);
    printf("\n");
    status = 0;
done:
    free(voices); free(owner); free(section_of); free(items); free(all); free(optimized); free(templates);
    for (uint32_t i = 0; i < adpcm_count; ++i) free(adpcm_data[i]);
    free(adpcm_data);
    return status;
}

int main(int argc, char **argv) {
    const char *source = NULL, *output = NULL; double gain = -6.4; int adpcm = 0, song = 0, trim = 0;
    uint32_t song_start = 0, song_period = 0, song_core_end = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--adpcm")) adpcm = 1;
        else if (!strcmp(argv[i], "--trim")) trim = 1;
        else if (!strcmp(argv[i], "--gain-db") && i + 1 < argc) gain = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--song") && i + 1 < argc) {
            unsigned long a = 0, p = 0, c = 0; int got = sscanf(argv[++i], "%lu:%lu:%lu", &a, &p, &c);
            if (got < 2 || !a || !p || a > UINT32_MAX / 4 || p > UINT32_MAX / 4 || c > UINT32_MAX / 4) { fprintf(stderr, "afx_vgm: --song CORE_START:PERIOD[:CORE_END] in ms\n"); return 2; }
            song = 1; song_start = (uint32_t)a; song_period = (uint32_t)p; song_core_end = (uint32_t)c;
        }
        else if (!source) source = argv[i]; else if (!output) output = argv[i]; else { fprintf(stderr, "usage: afx_vgm SOURCE.vgm|vgz OUTPUT.afx|OUTPUT.afm [--gain-db DB] [--adpcm] [--song CORE_START:PERIOD[:CORE_END]] [--trim]\n"); return 2; }
    }
    if (!source || !output || !isfinite(gain) || gain < -48 || gain > 24) return 2;
    parser_t parser = {0}; afx_c_event_t *events = NULL; afx_c_zone_t *zones = NULL; uint32_t count = 0, zone_count = 0;
    afx_c_event_t *optimized = NULL; afx_c_zone_t *templates = NULL; uint32_t template_count = 0;
    afx_c_output_t result = {0}; char *bank = NULL, *seek = NULL, *visual = NULL; uint8_t **adpcm_data = NULL; uint32_t adpcm_count = 0; int status = 1;
    if (parse_vgm(source, gain, &parser)) { fprintf(stderr, "afx_vgm: cannot parse %s\n", source); goto done; }
    if (compact(&parser, &events, &count, &zones, &zone_count) || !count || !zone_count) { fprintf(stderr, "afx_vgm: cannot lower source events\n"); goto done; }
    if (song) {
        if (compile_vgm_song(&parser, events, count, zones, zone_count, song_start, song_period, song_core_end, adpcm, trim, &result)) goto done;
        bank = with_suffix(output, ".afb");
        if (!bank || write_file(output, result.afm, result.afm_bytes) || write_file(bank, result.afb, result.afb_bytes)) goto done;
        printf("wrote %s (%u AFM, %u AFB)\n", output, result.afm_bytes, result.afb_bytes);
        status = 0; goto done;
    }
    if (trim) {
        uint32_t *reach = calloc(zone_count, sizeof(*reach));
        if (!reach || afx_c_sample_reach(events, count, zones, zone_count, TICK_RATE, reach)) { free(reach); fprintf(stderr, "afx_vgm: cannot measure the samples' reach\n"); goto done; }
        printf("%u sample frames never read, trimmed\n", afx_c_trim_zones(zones, zone_count, reach)); free(reach);
    }
    if (afx_c_optimize_events(events, count, zones, zone_count, &optimized, &templates, &template_count)) {
        fprintf(stderr, "afx_vgm: cannot optimize source events\n"); goto done;
    }
    free(events); free(zones); events = optimized; zones = templates; optimized = NULL; templates = NULL; zone_count = template_count;
    if (adpcm && encode_adpcm_zones(zones, zone_count, &adpcm_data, &adpcm_count)) { fprintf(stderr, "afx_vgm: ADPCM encoding failed\n"); goto done; }
    if (afx_c_compile_events(events, count, tick(parser.elapsed), TICK_RATE, zones, zone_count, &result)) { fprintf(stderr, "afx_vgm: AFX execution or asset limits exceeded\n"); goto done; }
    bank = with_suffix(output, ".afb"); seek = with_suffix(output, ".afc"); visual = with_suffix(output, ".afv");
    if (!bank || !seek || !visual || write_file(output, result.afx, result.afx_bytes) || write_file(bank, result.afb, result.afb_bytes) ||
        write_file(seek, result.afc, result.afc_bytes) || write_file(visual, result.afv, result.afv_bytes)) goto done;
    printf("wrote %s (%u AFX, %u AFB, %u AFC, %u AFV)\n", output, result.afx_bytes, result.afb_bytes, result.afc_bytes, result.afv_bytes);
    status = 0;
done:
    free(bank); free(seek); free(visual); afx_c_output_free(&result); free(events); free(zones); free(optimized); free(templates);
    for (uint32_t i = 0; i < adpcm_count; ++i) free(adpcm_data[i]);
    free(adpcm_data); parser_free(&parser);
    return status;
}
