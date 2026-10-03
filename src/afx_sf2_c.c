#include "afx_sf2_c.h"
#include "afx_sample_c.h"

#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PHDR_BYTES = 38, BAG_BYTES = 4, GEN_BYTES = 4, MOD_BYTES = 10, INST_BYTES = 22,
    SHDR_BYTES = 46, GEN_INITIAL_FILTER_FC = 8, GEN_INITIAL_FILTER_Q = 9,
    GEN_MOD_ENV_TO_FILTER_FC = 11, GEN_REVERB_SEND = 16, GEN_PAN = 17,
    GEN_MOD_ENV_ATTACK = 26, GEN_MOD_ENV_DECAY = 28, GEN_MOD_ENV_SUSTAIN = 29,
    GEN_MOD_ENV_RELEASE = 30, GEN_MOD_ENV_KEY_DECAY = 32,
    GEN_VOL_ENV_ATTACK = 34, GEN_VOL_ENV_DECAY = 36, GEN_VOL_ENV_SUSTAIN = 37,
    GEN_VOL_ENV_RELEASE = 38, GEN_INSTRUMENT = 41, GEN_KEY_RANGE = 43,
    GEN_VELOCITY_RANGE = 44, GEN_COARSE_TUNE = 51, GEN_FINE_TUNE = 52,
    GEN_INITIAL_ATTENUATION = 48, GEN_SAMPLE_ID = 53, GEN_SAMPLE_MODES = 54,
    GEN_OVERRIDE_ROOT = 58
};

enum { CTRL_FILTER_FC, CTRL_FILTER_Q, CTRL_MOD_FILTER, CTRL_MOD_ATTACK,
       CTRL_MOD_DECAY, CTRL_MOD_SUSTAIN, CTRL_MOD_RELEASE, CTRL_MOD_KEY_DECAY,
       CTRL_VOL_ATTACK, CTRL_VOL_DECAY, CTRL_VOL_SUSTAIN, CTRL_VOL_RELEASE,
       CTRL_ATTENUATION, CTRL_PAN, CTRL_REVERB, CTRL_COUNT };
enum { MODULATOR_MAX = 48 };

typedef struct {
    const uint8_t *smpl, *phdr, *pbag, *pmod, *pgen, *inst, *ibag, *imod, *igen, *shdr;
    uint32_t smpl_frames, phdr_count, pbag_count, pgen_count, inst_count,
             pmod_count, ibag_count, igen_count, imod_count, shdr_count;
} sf2_t;

typedef struct {
    int instrument, sample, root, mode;
    int key_lo, key_hi, velocity_lo, velocity_hi, coarse, fine;
    uint32_t controls_mask;
    int controls[CTRL_COUNT];
    int velocity_attenuation_amount, velocity_filter_amount;
} controls_t;

typedef struct {
    uint16_t source, destination, amount_source, transform;
    int16_t amount;
} modulator_t;

typedef struct {
    int velocity_attenuation_amount, velocity_filter_amount;
    modulator_t items[MODULATOR_MAX];
    uint8_t count;
} modulators_t;

typedef struct {
    uint8_t *data;
    afx_c_sample_t sample;
    uint16_t loop_start, loop_end;
    int loop_capable;
    int ready;
} decoded_t;

typedef struct {
    afx_c_sf2_output_t *out;
    decoded_t *decoded;
    uint32_t decoded_count;
    const sf2_t *font;
    const afx_c_note_t *source;
    const afx_c_sf2_options_t *options;
    int matched;
} resolver_t;

static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static int16_t sle16(const uint8_t *p) { return (int16_t)le16(p); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int read_file(const char *path, uint8_t **out_data, uint32_t *out_bytes) {
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *data = NULL;
    if (!file || fseek(file, 0, SEEK_END) || (size = ftell(file)) < 12 ||
        (uint64_t)size > UINT32_MAX || fseek(file, 0, SEEK_SET)) goto failed;
    data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) goto failed;
    fclose(file); *out_data = data; *out_bytes = (uint32_t)size; return 0;
failed:
    if (file) fclose(file);
    free(data); return -1;
}

static int find_chunk(const uint8_t *data, uint32_t bytes, const char id[4],
                      const uint8_t **out_data, uint32_t *out_bytes) {
    if (bytes < 8) return -1;
    for (uint32_t at = 0; at <= bytes - 8u;) {
        uint32_t length = le32(data + at + 4), padded = length + (length & 1u);
        if (length > bytes - at - 8u || padded < length) return -1;
        if (!memcmp(data + at, id, 4)) {
            *out_data = data + at + 8; *out_bytes = length; return 0;
        }
        if (padded > bytes - at - 8u) return -1;
        at += 8u + padded;
    }
    return -1;
}

static int find_list(const uint8_t *data, uint32_t bytes, const char type[4],
                     const uint8_t **out_data, uint32_t *out_bytes) {
    if (bytes < 12) return -1;
    for (uint32_t at = 12; at <= bytes - 12u;) {
        uint32_t length = le32(data + at + 4), padded = length + (length & 1u);
        if (length < 4 || length > bytes - at - 8u || padded < length) return -1;
        if (!memcmp(data + at, "LIST", 4) && !memcmp(data + at + 8, type, 4)) {
            *out_data = data + at + 12; *out_bytes = length - 4; return 0;
        }
        if (padded > bytes - at - 8u) return -1;
        at += 8u + padded;
    }
    return -1;
}

static int load_font(const uint8_t *data, uint32_t bytes, sf2_t *font) {
    const uint8_t *sdta, *pdta;
    uint32_t sdta_bytes, pdta_bytes, chunk_bytes;
    if (bytes < 12 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "sfbk", 4) ||
        le32(data + 4) > bytes - 8u || find_list(data, bytes, "sdta", &sdta, &sdta_bytes) ||
        find_list(data, bytes, "pdta", &pdta, &pdta_bytes)) return -1;
    memset(font, 0, sizeof(*font));
    if (find_chunk(sdta, sdta_bytes, "smpl", &font->smpl, &chunk_bytes) || (chunk_bytes & 1u) ||
        find_chunk(pdta, pdta_bytes, "phdr", &font->phdr, &chunk_bytes) || chunk_bytes % PHDR_BYTES ||
        find_chunk(pdta, pdta_bytes, "pbag", &font->pbag, &chunk_bytes) || chunk_bytes % BAG_BYTES ||
        find_chunk(pdta, pdta_bytes, "pmod", &font->pmod, &chunk_bytes) || chunk_bytes % MOD_BYTES ||
        find_chunk(pdta, pdta_bytes, "pgen", &font->pgen, &chunk_bytes) || chunk_bytes % GEN_BYTES ||
        find_chunk(pdta, pdta_bytes, "inst", &font->inst, &chunk_bytes) || chunk_bytes % INST_BYTES ||
        find_chunk(pdta, pdta_bytes, "ibag", &font->ibag, &chunk_bytes) || chunk_bytes % BAG_BYTES ||
        find_chunk(pdta, pdta_bytes, "imod", &font->imod, &chunk_bytes) || chunk_bytes % MOD_BYTES ||
        find_chunk(pdta, pdta_bytes, "igen", &font->igen, &chunk_bytes) || chunk_bytes % GEN_BYTES ||
        find_chunk(pdta, pdta_bytes, "shdr", &font->shdr, &chunk_bytes) || chunk_bytes % SHDR_BYTES) return -1;
    find_chunk(sdta, sdta_bytes, "smpl", &font->smpl, &chunk_bytes); font->smpl_frames = chunk_bytes / 2u;
    find_chunk(pdta, pdta_bytes, "phdr", &font->phdr, &chunk_bytes); font->phdr_count = chunk_bytes / PHDR_BYTES;
    find_chunk(pdta, pdta_bytes, "pbag", &font->pbag, &chunk_bytes); font->pbag_count = chunk_bytes / BAG_BYTES;
    find_chunk(pdta, pdta_bytes, "pmod", &font->pmod, &chunk_bytes); font->pmod_count = chunk_bytes / MOD_BYTES;
    find_chunk(pdta, pdta_bytes, "pgen", &font->pgen, &chunk_bytes); font->pgen_count = chunk_bytes / GEN_BYTES;
    find_chunk(pdta, pdta_bytes, "inst", &font->inst, &chunk_bytes); font->inst_count = chunk_bytes / INST_BYTES;
    find_chunk(pdta, pdta_bytes, "ibag", &font->ibag, &chunk_bytes); font->ibag_count = chunk_bytes / BAG_BYTES;
    find_chunk(pdta, pdta_bytes, "imod", &font->imod, &chunk_bytes); font->imod_count = chunk_bytes / MOD_BYTES;
    find_chunk(pdta, pdta_bytes, "igen", &font->igen, &chunk_bytes); font->igen_count = chunk_bytes / GEN_BYTES;
    find_chunk(pdta, pdta_bytes, "shdr", &font->shdr, &chunk_bytes); font->shdr_count = chunk_bytes / SHDR_BYTES;
    return font->phdr_count > 1 && font->pbag_count > 1 && font->inst_count > 1 &&
           font->ibag_count > 1 && font->shdr_count > 1 ? 0 : -1;
}

static controls_t controls_default(void) {
    return (controls_t){.instrument = -1, .sample = -1, .root = -1, .mode = -1,
                        .key_lo = 0, .key_hi = 127, .velocity_lo = 0, .velocity_hi = 127};
}

static void control_set(controls_t *out, unsigned id, int value) {
    out->controls_mask |= 1u << id; out->controls[id] = value;
}

static int control_get(const controls_t *controls, unsigned id, int fallback) {
    return controls->controls_mask & (1u << id) ? controls->controls[id] : fallback;
}

/* SF2 defaults which participate when a preset supplies a generator but its
 * instrument does not.  Other generators have an additive zero default. */
static int control_default(unsigned id) {
    switch (id) {
        case CTRL_FILTER_FC: return 13500;
        case CTRL_MOD_ATTACK: case CTRL_MOD_DECAY: case CTRL_MOD_RELEASE:
        case CTRL_VOL_ATTACK: case CTRL_VOL_DECAY: case CTRL_VOL_RELEASE: return -12000;
        default: return 0;
    }
}

static controls_t read_controls(const uint8_t *gens, uint32_t count, uint32_t first, uint32_t last) {
    controls_t out = controls_default();
    if (first > last || last > count) { out.key_lo = 1; out.key_hi = 0; return out; }
    for (uint32_t index = first; index < last; ++index) {
        const uint8_t *gen = gens + index * GEN_BYTES;
        switch (le16(gen)) {
            case GEN_INSTRUMENT: out.instrument = le16(gen + 2); break;
            case GEN_SAMPLE_ID: out.sample = le16(gen + 2); break;
            case GEN_KEY_RANGE: out.key_lo = gen[2]; out.key_hi = gen[3]; break;
            case GEN_VELOCITY_RANGE: out.velocity_lo = gen[2]; out.velocity_hi = gen[3]; break;
            case GEN_COARSE_TUNE: out.coarse += sle16(gen + 2); break;
            case GEN_FINE_TUNE: out.fine += sle16(gen + 2); break;
            case GEN_SAMPLE_MODES: out.mode = le16(gen + 2); break;
            case GEN_OVERRIDE_ROOT: out.root = le16(gen + 2) <= 127 ? le16(gen + 2) : -1; break;
            case GEN_INITIAL_FILTER_FC: control_set(&out, CTRL_FILTER_FC, sle16(gen + 2)); break;
            case GEN_INITIAL_FILTER_Q: control_set(&out, CTRL_FILTER_Q, sle16(gen + 2)); break;
            case GEN_MOD_ENV_TO_FILTER_FC: control_set(&out, CTRL_MOD_FILTER, sle16(gen + 2)); break;
            case GEN_REVERB_SEND: control_set(&out, CTRL_REVERB, sle16(gen + 2)); break;
            case GEN_PAN: control_set(&out, CTRL_PAN, sle16(gen + 2)); break;
            case GEN_MOD_ENV_ATTACK: control_set(&out, CTRL_MOD_ATTACK, sle16(gen + 2)); break;
            case GEN_MOD_ENV_DECAY: control_set(&out, CTRL_MOD_DECAY, sle16(gen + 2)); break;
            case GEN_MOD_ENV_SUSTAIN: control_set(&out, CTRL_MOD_SUSTAIN, sle16(gen + 2)); break;
            case GEN_MOD_ENV_RELEASE: control_set(&out, CTRL_MOD_RELEASE, sle16(gen + 2)); break;
            case GEN_MOD_ENV_KEY_DECAY: control_set(&out, CTRL_MOD_KEY_DECAY, sle16(gen + 2)); break;
            case GEN_VOL_ENV_ATTACK: control_set(&out, CTRL_VOL_ATTACK, sle16(gen + 2)); break;
            case GEN_VOL_ENV_DECAY: control_set(&out, CTRL_VOL_DECAY, sle16(gen + 2)); break;
            case GEN_VOL_ENV_SUSTAIN: control_set(&out, CTRL_VOL_SUSTAIN, sle16(gen + 2)); break;
            case GEN_VOL_ENV_RELEASE: control_set(&out, CTRL_VOL_RELEASE, sle16(gen + 2)); break;
            case GEN_INITIAL_ATTENUATION: control_set(&out, CTRL_ATTENUATION, sle16(gen + 2)); break;
        }
    }
    return out;
}

static controls_t combine(controls_t base, controls_t local) {
    if (local.key_lo > base.key_lo) base.key_lo = local.key_lo;
    if (local.key_hi < base.key_hi) base.key_hi = local.key_hi;
    if (local.velocity_lo > base.velocity_lo) base.velocity_lo = local.velocity_lo;
    if (local.velocity_hi < base.velocity_hi) base.velocity_hi = local.velocity_hi;
    if (local.instrument >= 0) base.instrument = local.instrument;
    if (local.sample >= 0) base.sample = local.sample;
    if (local.root >= 0) base.root = local.root;
    if (local.mode >= 0) base.mode = local.mode;
    base.coarse += local.coarse; base.fine += local.fine;
    return base;
}

/* SF2 globals are defaults for their local zones. */
static controls_t overlay(controls_t base, const controls_t *local) {
    controls_t out = combine(base, *local);
    for (unsigned i = 0; i < CTRL_COUNT; ++i)
        if (local->controls_mask & (1u << i)) {
            out.controls_mask |= 1u << i;
            out.controls[i] = local->controls[i];
        }
    return out;
}

/* Preset and instrument controls add after each level has resolved globals. */
static controls_t add_controls(controls_t base, const controls_t *local) {
    controls_t out = combine(base, *local);
    for (unsigned i = 0; i < CTRL_COUNT; ++i) {
        if ((base.controls_mask & (1u << i)) && !(local->controls_mask & (1u << i))) {
            out.controls[i] = base.controls[i] + control_default(i);
            out.controls_mask |= 1u << i;
        }
        if (local->controls_mask & (1u << i)) {
            out.controls[i] = (base.controls_mask & (1u << i) ? base.controls[i] : 0) + local->controls[i];
            out.controls_mask |= 1u << i;
        }
    }
    return out;
}

/* These are the two standard SF2 modulators whose results AICA can lower
 * exactly into a NOTE's gain and filter setup.  Other arbitrary SF2 graphs
 * deliberately remain outside this compact offline reader. */
static int modulator_store(modulators_t *out, const modulator_t *item) {
    for (uint32_t i = 0; i < out->count; ++i)
        if (out->items[i].source == item->source && out->items[i].destination == item->destination &&
            out->items[i].amount_source == item->amount_source && out->items[i].transform == item->transform) {
            out->items[i] = *item; return 0;
        }
    if (out->count == MODULATOR_MAX) return -1;
    out->items[out->count++] = *item; return 0;
}

static int modulator_append(modulators_t *out, const modulators_t *items) {
    for (uint32_t i = 0; i < items->count; ++i) {
        if (out->count == MODULATOR_MAX) return -1;
        out->items[out->count++] = items->items[i];
    }
    return 0;
}

static int modulator_override(const uint8_t *mods, uint32_t mod_count,
                               const uint8_t *bags, uint32_t bag, uint32_t bag_count,
                               modulators_t *out) {
    if (!mods || !bags || !out || bag + 1u >= bag_count) return -1;
    uint32_t first = le16(bags + bag * BAG_BYTES + 2),
             last = le16(bags + (bag + 1u) * BAG_BYTES + 2);
    if (first > last || last > mod_count) return -1;
    for (uint32_t i = first; i < last; ++i) {
        const uint8_t *mod = mods + i * MOD_BYTES;
        uint16_t source = le16(mod), destination = le16(mod + 2),
                 amount_source = le16(mod + 6), transform = le16(mod + 8);
        int amount = sle16(mod + 4);
        modulator_t item = {source, destination, amount_source, transform, (int16_t)amount};
        if (!amount_source && !transform && source == 1282 && destination == GEN_INITIAL_ATTENUATION)
            out->velocity_attenuation_amount = amount;
        else if (!amount_source && !transform && source == 258 && destination == GEN_INITIAL_FILTER_FC)
            out->velocity_filter_amount = amount;
        else if (amount && modulator_store(out, &item)) return -1;
    }
    return 0;
}

/* AFBM chooses whether a source's graph participates. The result is sampled
 * once at NOTE-on and lowered into the existing setup dictionary; no SF2 or
 * MIDI state reaches the console. Curved and LFO sources are time functions,
 * so this static path deliberately does not guess at them. */
static int modulator_source(const afx_c_note_t *note, uint16_t source, double *out) {
    unsigned index = source & 0x7fu, curve = source >> 10;
    double value;
    if (curve || (!index && !(source & 0x80u))) return 0;
    if (source & 0x80u) value = note->controllers[index] / 127.0;
    else switch (index) {
        case 2: value = note->velocity / 127.0; break;
        case 3: value = note->key / 127.0; break;
        case 10: value = note->poly_pressure / 127.0; break;
        case 13: value = note->channel_pressure / 127.0; break;
        case 14: value = ((note->controller_state >> 25) & 16383u) / 16383.0; break;
        case 16: value = note->pitch_sensitivity / 127.0; break;
        default: return 0;
    }
    if (source & 0x100u) value = 1.0 - value;
    if (source & 0x200u) value = 2.0 * value - 1.0;
    *out = value; return 1;
}

static void control_add(controls_t *out, unsigned id, int delta) {
    control_set(out, id, control_get(out, id, control_default(id)) + delta);
}

static void apply_modulators(controls_t *out, const afx_c_note_t *note,
                             const modulators_t *modulators, int enabled) {
    if (!enabled) return;
    for (uint32_t i = 0; i < modulators->count; ++i) {
        const modulator_t *item = modulators->items + i;
        double source, amount = 1.0;
        int delta;
        /* The two standard velocity rules retain their higher-fidelity SF2
         * curve in sf2_mix/lower_zone. */
        if ((item->source == 1282 && item->destination == GEN_INITIAL_ATTENUATION) ||
            (item->source == 258 && item->destination == GEN_INITIAL_FILTER_FC) || item->transform ||
            ((item->source == 135 || item->source == 139) && item->destination == GEN_INITIAL_ATTENUATION) ||
            (item->source == 138 && item->destination == GEN_PAN) ||
            !modulator_source(note, item->source, &source) ||
            (item->amount_source && !modulator_source(note, item->amount_source, &amount))) continue;
        delta = (int)lround(item->amount * source * amount);
        switch (item->destination) {
            case GEN_INITIAL_FILTER_FC: control_add(out, CTRL_FILTER_FC, delta); break;
            case GEN_MOD_ENV_TO_FILTER_FC: control_add(out, CTRL_MOD_FILTER, delta); break;
            case GEN_REVERB_SEND: control_add(out, CTRL_REVERB, delta); break;
            case GEN_PAN: control_add(out, CTRL_PAN, delta); break;
            case GEN_MOD_ENV_ATTACK: control_add(out, CTRL_MOD_ATTACK, delta); break;
            case GEN_MOD_ENV_DECAY: control_add(out, CTRL_MOD_DECAY, delta); break;
            case GEN_MOD_ENV_SUSTAIN: control_add(out, CTRL_MOD_SUSTAIN, delta); break;
            case GEN_MOD_ENV_RELEASE: control_add(out, CTRL_MOD_RELEASE, delta); break;
            case GEN_VOL_ENV_ATTACK: control_add(out, CTRL_VOL_ATTACK, delta); break;
            case GEN_VOL_ENV_DECAY: control_add(out, CTRL_VOL_DECAY, delta); break;
            case GEN_VOL_ENV_SUSTAIN: control_add(out, CTRL_VOL_SUSTAIN, delta); break;
            case GEN_VOL_ENV_RELEASE: control_add(out, CTRL_VOL_RELEASE, delta); break;
            case GEN_INITIAL_ATTENUATION: control_add(out, CTRL_ATTENUATION, delta); break;
            case GEN_COARSE_TUNE: out->coarse += delta; break;
            case GEN_FINE_TUNE: out->fine += delta; break;
        }
    }
}

static int matches(const controls_t *controls, const afx_c_note_t *note) {
    return controls->key_lo <= note->key && note->key <= controls->key_hi &&
           controls->velocity_lo <= note->velocity && note->velocity <= controls->velocity_hi;
}

static int ensure_sample(resolver_t *resolver, const controls_t *controls, uint32_t sample_id,
                         afx_c_sample_t *out) {
    const sf2_t *font = resolver->font;
    if (sample_id >= resolver->decoded_count) return -1;
    const uint8_t *header = font->shdr + sample_id * SHDR_BYTES;
    uint32_t start = le32(header + 20), end = le32(header + 24), loop_start = le32(header + 28),
             loop_end = le32(header + 32), rate = le32(header + 36);
    if (!rate || start >= end || end > font->smpl_frames || (le16(header + 44) & 0x8000u)) return -1;
    int source_loop = loop_start >= start && loop_start < loop_end && loop_end <= end;
    int keep_loop = (controls->mode & 1) && source_loop;
    /* A looped and a one-shot use of one SF2 sample need independent cached
     * encodings: only the looped form may discard its post-loop tail. */
    decoded_t *decoded = resolver->decoded + sample_id * 2u + (unsigned)keep_loop;
    if (!decoded->ready) {
        uint32_t source_frames = end - start;
        uint64_t wanted_rate = rate;
        if (resolver->options->sample_rate_cap && wanted_rate > resolver->options->sample_rate_cap)
            wanted_rate = resolver->options->sample_rate_cap;
        /* Match the established offline layout, not merely AICA's raw
         * register maximum. One-shots append a 256-frame silent safety tail;
         * looped sources retain a small rounding margin around LEA. */
        uint32_t frame_cap = keep_loop ? 65500u : 65279u;
        if ((uint64_t)source_frames * wanted_rate > frame_cap * (uint64_t)rate)
            wanted_rate = (uint64_t)frame_cap * rate / source_frames;
        if (!wanted_rate) return -1;
        uint8_t *pcm16 = NULL, *encoded = NULL, format = 0;
        uint32_t frames = 0;
        uint32_t encoded_bytes = 0;
        if (afx_c_resample_pcm16(font->smpl + 2u * start, source_frames, rate,
                                 (uint32_t)wanted_rate, &pcm16, &frames) || frames > 65535u) return -1;
        uint32_t local_start = loop_start > start ? (uint32_t)nearbyint((double)(loop_start - start) * wanted_rate / rate) : 0;
        uint32_t local_end = loop_end > start ? (uint32_t)nearbyint((double)(loop_end - start) * wanted_rate / rate) : 0;
        int loop_capable = local_start < local_end && local_end <= frames;
        if (keep_loop && loop_capable && resolver->options->loop_ms &&
            frames > (uint64_t)wanted_rate * resolver->options->loop_ms / 1000u) {
            uint32_t target = local_start + (uint32_t)((uint64_t)wanted_rate * resolver->options->loop_ms / 1000u);
            uint32_t margin = (uint32_t)((uint64_t)wanted_rate * resolver->options->loop_ms / 4000u);
            uint32_t low, high, best, best_cost;
            if (target > local_end) target = local_end;
            low = target > margin ? target - margin : 0;
            if (low < local_start + 2u) low = local_start + 2u;
            high = target > UINT32_MAX - margin ? local_end : target + margin;
            if (high > local_end) high = local_end;
            if (low <= high) {
                int first = (int16_t)((uint16_t)pcm16[2u * local_start] | (uint16_t)pcm16[2u * local_start + 1u] << 8);
                int second = (int16_t)((uint16_t)pcm16[2u * (local_start + 1u)] | (uint16_t)pcm16[2u * (local_start + 1u) + 1u] << 8);
                best = low; best_cost = UINT32_MAX;
                for (uint32_t frame = low; frame <= high; ++frame) {
                    int previous = (int16_t)((uint16_t)pcm16[2u * (frame - 1u)] | (uint16_t)pcm16[2u * (frame - 1u) + 1u] << 8);
                    int before = (int16_t)((uint16_t)pcm16[2u * (frame - 2u)] | (uint16_t)pcm16[2u * (frame - 2u) + 1u] << 8);
                    uint32_t cost = (uint32_t)abs(previous - first) + (uint32_t)abs((previous - before) - (second - first));
                    if (cost < best_cost) { best = frame; best_cost = cost; }
                }
                local_end = best;
            }
        }
        if (keep_loop && loop_capable) {
            /* AICA re-enters at the supplied loop point, so samples following
             * it are unreachable. Keeping them was the reason the C export of
             * the licensed solo cello exceeded AICA RAM. */
            frames = local_end;
        }
        /* A source with a legal loop must not receive automatic ADPCM: a later
         * SF2 zone may turn that loop on, and AICA's predictor seam needs an
         * explicit capture test. */
        if (!keep_loop) {
            uint32_t fade, original_frames = frames;
            uint8_t *tailed;
            if (frames > 65535u - 256u) { free(pcm16); return -1; }
            tailed = realloc(pcm16, (size_t)(frames + 256u) * 2u);
            if (!tailed) { free(pcm16); return -1; }
            pcm16 = tailed; memset(pcm16 + frames * 2u, 0, 512u);
            fade = frames < wanted_rate / 5u ? frames : (uint32_t)(wanted_rate / 5u);
            if (!fade) fade = 1;
            for (uint32_t frame = 0; frame < fade; ++frame) {
                uint32_t offset = 2u * (original_frames - fade + frame);
                int value = (int16_t)((uint16_t)pcm16[offset] | (uint16_t)pcm16[offset + 1u] << 8);
                value = (int)lround((double)value * (fade - frame - 1u) / fade);
                pcm16[offset] = (uint8_t)value; pcm16[offset + 1u] = (uint8_t)(value >> 8);
            }
            local_start = original_frames; local_end = original_frames + 255u;
            frames += 256u;
        }
        if (afx_c_encode_sample(pcm16, frames, keep_loop && loop_capable, resolver->options->sample_format,
                                &encoded, &encoded_bytes, &format)) {
            free(pcm16); return -1;
        }
        free(pcm16);
        uint32_t output_rate = (uint32_t)wanted_rate;
        /* sample_rate is retained in the AFB and the common pitch lowering
         * compensates it once.  Baking rate_tune here as well transposes every
         * resampled sample by a second, erroneous factor. */
        int tuning = (int)(int8_t)header[41];
        if (tuning < INT16_MIN || tuning > INT16_MAX) { free(encoded); return -1; }
        decoded->data = encoded;
        decoded->sample = (afx_c_sample_t){encoded, encoded_bytes, frames, format,
                                           header[40] <= 127 ? header[40] : 60,
                                           1u, local_start, local_end,
                                           (int16_t)tuning, output_rate};
        decoded->loop_start = (uint16_t)local_start;
        decoded->loop_end = (uint16_t)(loop_capable ? local_end - 1u : 0);
        decoded->loop_capable = loop_capable;
        decoded->ready = 1;
    }
    *out = decoded->sample;
    if (keep_loop && decoded->loop_capable) {
        out->loop = 1; out->loop_start = decoded->loop_start; out->loop_end = decoded->loop_end;
    }
    if (controls->root >= 0) out->root_key = (uint8_t)controls->root;
    int tuning = (int)out->tuning_cents + controls->coarse * 100 + controls->fine;
    if (tuning < INT16_MIN || tuning > INT16_MAX) return -1;
    out->tuning_cents = (int16_t)tuning;
    return 0;
}

static const double ar_time_ms[64] = {100000,100000,8100,6900,6000,4800,4000,3400,3000,2400,2000,
    1700,1500,1200,1000,860,760,600,500,430,380,300,250,220,190,150,130,110,95,76,63,55,
    47,38,31,27,24,19,15,13,12,9.4,7.9,6.8,6,4.7,3.8,3.4,3,2.4,2,1.8,1.6,1.3,1.1,
    .93,.85,.65,.53,.44,.4,.35,0,0};
static const double dr_time_ms[64] = {100000,100000,118200,101300,88600,70900,59100,50700,44300,
    35500,29600,25300,22200,17700,14800,12700,11100,8900,7400,6300,5500,4400,3700,3200,
    2800,2200,1800,1600,1400,1100,920,790,690,550,460,390,340,270,230,200,170,140,110,
    98,85,68,57,49,43,34,28,25,22,18,14,12,11,8.5,7.1,6.1,5.4,4.3,3.6,3.1};

static int aica_rate(int timecents, const double table[64]) {
    double target = 1000.0 * pow(2.0, timecents / 1200.0);
    int best = 1;
    for (int rate = 2; rate < 31; ++rate)
        if (fabs(log(table[2 * rate] / target)) < fabs(log(table[2 * best] / target))) best = rate;
    return best;
}

static uint16_t filter_level(double cents) {
    double frequency = 8.176 * pow(2.0, fmax(-16000, fmin(16000, cents)) / 1200.0);
    double coefficient, mantissa;
    int exponent;
    if (frequency < 20) frequency = 20;
    /* AICA's FEG level is a 13-bit word.  Above the useful audio range the
       SF2 default is an open filter, not a value with bit 13 set. */
    if (frequency >= 18000) return 0x1ff7;
    coefficient = 2.0 * sin(acos(-1.0) * frequency / 44100.0);
    exponent = (int)floor(log2(coefficient)) + 16;
    if (exponent < 0) exponent = 0;
    if (exponent > 15) exponent = 15;
    /* Python's reference author uses round(), i.e. IEEE ties-to-even.  Keep
       the default floating-point rounding mode here so an AFBM lowered by C
       produces the same cutoff word instead of an adjacent FEG level. */
    mantissa = nearbyint(coefficient * pow(2.0, 25 - exponent));
    if (mantissa < 0) mantissa = 0;
    if (mantissa > 0x1ff7) mantissa = 0x1ff7;
    int level = exponent * 512 + (int)mantissa - 512;
    return (uint16_t)(level > 0x1ff7 ? 0x1ff7 : level);
}

static int filter_rate(int timecents, int distance) {
    if (!distance) return 0;
    double target = 1000.0 * pow(2.0, fmax(-12000, fmin(16000, timecents)) / 1200.0);
    int best = 1;
    for (int rate = 2; rate < 32; ++rate)
        if (fabs(log(dr_time_ms[2 * rate] * abs(distance) / 1024.0 / target)) <
            fabs(log(dr_time_ms[2 * best] * abs(distance) / 1024.0 / target))) best = rate;
    return best;
}

/* SF2 source pan and MIDI channel pan are one combined stereo position.  The
 * MIDI reader snapshots CC10 at NOTE-on, so lowering remains entirely
 * offline.  `source_pan=ignore` deliberately suppresses both, matching the
 * explicit map policy rather than silently keeping MIDI pan. */
static int sf2_pan(const controls_t *controls, const afx_c_note_t *note,
                   const afx_c_sf2_options_t *options) {
    int pan = control_get(controls, CTRL_PAN, 0);
    if (options->source_pan && note->controller_state >> 39) {
        int channel_pan = (int)((note->controller_state >> 18) & 127u);
        pan += (int)lround((channel_pan - 64) * 500.0 / 63.0);
    }
    if (pan < -500) pan = -500;
    if (pan > 500) pan = 500;
    return pan;
}

static uint16_t sf2_mix(const controls_t *controls, const afx_c_note_t *note,
                        const afx_c_sf2_options_t *options) {
    int pan = sf2_pan(controls, note, options);
    double angle, loud, source, velocity_attenuation, pan_attenuation, attenuation;
    angle = (pan + 500) * acos(-1.0) / 2000.0;
    loud = fmax(cos(angle), sin(angle));
    /* SF2 initialAttenuation is already expressed in centibels.  Applying
       the legacy E-mu 0.04 dB scaling here turns quiet string zones into
       effectively muted AICA voices. */
    source = control_get(controls, CTRL_ATTENUATION, 0) *
             (options->gain_model == AFX_C_SF2_GAIN_FLUIDSYNTH2 ? 4.0 : 1.0);
    /* FluidSynth's standard SF2 velocity modulator is -40 log10(v/127) dB. */
    velocity_attenuation = -4000.0 * log10((double)note->velocity / 127.0);
    if (options->gain_model == AFX_C_SF2_GAIN_FLUIDSYNTH2)
        velocity_attenuation *= controls->velocity_attenuation_amount / 960.0;
    pan_attenuation = -2000.0 * log10(loud * sqrt(2.0));
    attenuation = options->gain_bias_centibels + note->attenuation_offset_centibels + source +
                  velocity_attenuation + pan_attenuation;
    /* SF2's default volume and expression modulators are controller state at
     * NOTE-on. Later automation is lowered separately as PATCH commands. */
    if (note->controller_state >> 39) {
        unsigned volume = (note->controller_state >> 4) & 127u;
        unsigned expression = (note->controller_state >> 11) & 127u;
        if (!volume || !expression) attenuation = 10200.0;
        else attenuation += (options->gain_model == AFX_C_SF2_GAIN_FLUIDSYNTH2 ? 2.0 : 1.0) *
                            -2000.0 * log10((double)volume * expression / (127.0 * 127.0));
    }
    /* Convert the SF2 centibel model to AICA's TL calibration. */
    int tl = (int)lround(attenuation / (2000.0 * log10(2.0) / 16.0));
    if (tl < 0) tl = 0;
    if (tl > 255) tl = 255;
    int lpf = options->gain_model == AFX_C_SF2_GAIN_FLUIDSYNTH2 ?
              (int)lround(control_get(controls, CTRL_FILTER_Q, 0) / 7.5 + 4.0) : 0x24;
    if (lpf < 0) lpf = 0;
    if (lpf > 15) lpf = 15;
    return (uint16_t)(tl << 8 | lpf);
}

static afx_c_zone_t lower_zone(const afx_c_sample_t *sample, const controls_t *controls,
                                const afx_c_note_t *note, const afx_c_sf2_options_t *options) {
    int pan = sf2_pan(controls, note, options);
    /* SF2's velocity-to-filter modulator is fractional.  Keep it fractional
       until it becomes AICA's cutoff word: the Python reference does this
       too, and truncating here changes selected FEG values by one. */
    double cutoff = control_get(controls, CTRL_FILTER_FC, 13500) + options->filter_offset_cents +
                    (double)controls->velocity_filter_amount * (127 - note->velocity) / 128.0;
    int mod_filter = control_get(controls, CTRL_MOD_FILTER, 0);
    int sustain = control_get(controls, CTRL_MOD_SUSTAIN, 0);
    double peak = cutoff + mod_filter;
    double filter_sustain = cutoff + mod_filter * (1.0 - fmin(1000, fmax(0, sustain)) / 1000.0);
    int vol_sustain = control_get(controls, CTRL_VOL_SUSTAIN, 0);
    int dl = (int)lround(vol_sustain / (100.0 * log10(2.0)));
    if (dl < 0) dl = 0;
    if (dl > 31) dl = 31;
    double angle = (pan + 500) * acos(-1.0) / 2000.0;
    double loud = fmax(cos(angle), sin(angle)), soft = fmin(cos(angle), sin(angle));
    int dipan = (int)lround(-20.0 * log10(fmax(soft / loud, 1e-9)) / (10.0 * log10(2.0)));
    if (dipan > 15) dipan = 15;
    afx_c_zone_t zone = {.sample = *sample, .key_min = 0, .key_max = 127,
        .velocity_min = 0, .velocity_max = 127, .bank_msb = note->bank_msb,
        .bank_lsb = note->bank_lsb, .program = note->program,
        .dsp_send = options->has_dsp_send ? options->dsp_send :
                    (options->source_reverb ? (uint8_t)(fmin(1000, fmax(0, control_get(controls, CTRL_REVERB, 0))) * 15 / 1000) << 4 : 0)};
    zone.setup_mask = (1u << AFX_FIELD_DIRECT) |
                      (1u << AFX_FIELD_FILTER_LEVEL0) | (1u << AFX_FIELD_FILTER_LEVEL1) |
                      (1u << AFX_FIELD_FILTER_LEVEL2) | (1u << AFX_FIELD_FILTER_LEVEL3) |
                      (1u << AFX_FIELD_FILTER_LEVEL4) | (1u << AFX_FIELD_FILTER_AD) |
                      (1u << AFX_FIELD_FILTER_DR);
    if (options->envelope_model == AFX_C_SF2_ENVELOPE_SF2) {
        zone.setup_mask |= (1u << AFX_FIELD_ENV_AD) | (1u << AFX_FIELD_ENV_DR);
        zone.setup[AFX_FIELD_ENV_AD] = (uint16_t)(aica_rate(control_get(controls, CTRL_VOL_ATTACK, -12000), ar_time_ms) |
            aica_rate(control_get(controls, CTRL_VOL_DECAY, -12000), dr_time_ms) << 6);
        zone.setup[AFX_FIELD_ENV_DR] = (uint16_t)(aica_rate(control_get(controls, CTRL_VOL_RELEASE, -12000), dr_time_ms) | dl << 5 | 15 << 10);
    }
    /* DIRECT's high nibble is DISDL, not filter resonance.  SF2's initial
       filter Q has no equivalent at this register; leave the direct path at
       full level and retain SF2 pan in DIPAN. */
    uint16_t direct = options->has_direct ? options->direct : 0x0f10;
    /* `direct=` selects the direct-path/Q baseline; it does not freeze the
     * low pan bits when source_pan is applied.  This is the same split used
     * by the former Python author: keep Q, then lower SF2 + MIDI CC10 pan. */
    if (options->source_pan)
        direct = (uint16_t)((direct & 0xff00u) | dipan | (pan <= 0 ? 0x10 : 0));
    zone.setup[AFX_FIELD_DIRECT] = direct;
    if (options->has_lfo) {
        zone.setup_mask |= 1u << AFX_FIELD_LFO;
        zone.setup[AFX_FIELD_LFO] = options->lfo;
    }
    if (options->filter_model == AFX_C_SF2_FILTER_NONE) {
        for (uint32_t field = AFX_FIELD_FILTER_LEVEL0; field <= AFX_FIELD_FILTER_LEVEL4; ++field)
            zone.setup[field] = 0x1fffu;
        zone.setup[AFX_FIELD_FILTER_AD] = zone.setup[AFX_FIELD_FILTER_DR] = 0;
    } else {
        zone.setup[AFX_FIELD_FILTER_LEVEL0] = filter_level(cutoff);
        zone.setup[AFX_FIELD_FILTER_LEVEL1] = options->filter_model == AFX_C_SF2_FILTER_STATIC ? zone.setup[AFX_FIELD_FILTER_LEVEL0] : filter_level(peak);
        zone.setup[AFX_FIELD_FILTER_LEVEL2] = zone.setup[AFX_FIELD_FILTER_LEVEL3] =
            options->filter_model == AFX_C_SF2_FILTER_STATIC ? zone.setup[AFX_FIELD_FILTER_LEVEL0] : filter_level(filter_sustain);
        zone.setup[AFX_FIELD_FILTER_LEVEL4] = zone.setup[AFX_FIELD_FILTER_LEVEL0];
        zone.setup[AFX_FIELD_FILTER_AD] = options->filter_model == AFX_C_SF2_FILTER_STATIC ? 0 :
            (uint16_t)(filter_rate(control_get(controls, CTRL_MOD_ATTACK, -12000), (int)lround(peak - cutoff)) << 8 |
            filter_rate(control_get(controls, CTRL_MOD_DECAY, -12000), (int)lround(peak - filter_sustain)));
        zone.setup[AFX_FIELD_FILTER_DR] = options->filter_model == AFX_C_SF2_FILTER_STATIC ? 0 :
            (uint16_t)filter_rate(control_get(controls, CTRL_MOD_RELEASE, -12000), (int)lround(peak - cutoff));
    }
    return zone;
}

static int same_zone(const afx_c_zone_t *zone, const afx_c_zone_t *candidate) {
    return zone->sample.data == candidate->sample.data && zone->sample.root_key == candidate->sample.root_key &&
           zone->sample.loop == candidate->sample.loop && zone->sample.loop_start == candidate->sample.loop_start &&
           zone->sample.loop_end == candidate->sample.loop_end && zone->sample.tuning_cents == candidate->sample.tuning_cents &&
           zone->dsp_send == candidate->dsp_send && zone->setup_mask == candidate->setup_mask &&
           !memcmp(zone->setup, candidate->setup, sizeof(zone->setup));
}

static void copy_sample_name(char out[AFX_C_SAMPLE_NAME_BYTES], const sf2_t *font,
                             uint32_t sample_id) {
    const uint8_t *source = font->shdr + sample_id * SHDR_BYTES;
    unsigned bytes = 0;
    for (; bytes + 1u < AFX_C_SAMPLE_NAME_BYTES && source[bytes]; ++bytes)
        out[bytes] = source[bytes] >= 32 && source[bytes] <= 126 ? (char)source[bytes] : '_';
    while (bytes && out[bytes - 1u] == ' ') --bytes;
    out[bytes] = 0;
    if (!bytes) snprintf(out, AFX_C_SAMPLE_NAME_BYTES, "sample-%u", sample_id);
}

static int append_note(resolver_t *resolver, const controls_t *controls,
                       const modulators_t *modulators) {
    uint16_t sample_type;
    afx_c_sample_t sample;
    if (controls->sample < 0 || (uint32_t)controls->sample >= resolver->decoded_count) return -1;
    sample_type = le16(resolver->font->shdr + (uint32_t)controls->sample * SHDR_BYTES + 44) & 0x7fffu;
    if ((resolver->options->channel == AFX_C_SF2_LEFT && sample_type == 2) ||
        (resolver->options->channel == AFX_C_SF2_RIGHT && sample_type == 4)) return 0;
    controls_t selected = *controls;
    apply_modulators(&selected, resolver->source, modulators, resolver->options->source_modulators);
    if (ensure_sample(resolver, &selected, (uint32_t)selected.sample, &sample)) return -1;
    /* A one-side source is a mono memory-saving rendition, not hard-left or
     * hard-right stereo.  AICA receives it at centre pan. */
    if (resolver->options->channel && sample_type != 1) control_set(&selected, CTRL_PAN, 0);
    if (!resolver->options->source_pan) control_set(&selected, CTRL_PAN, 0);
    afx_c_zone_t candidate = lower_zone(&sample, &selected, resolver->source, resolver->options);
    uint32_t zone = 0;
    while (zone < resolver->out->zone_count && !same_zone(resolver->out->zones + zone, &candidate)) ++zone;
    if (zone == resolver->out->zone_count) {
        if (zone == UINT16_MAX) return -1;
        afx_c_zone_t *grown = realloc(resolver->out->zones, (size_t)(zone + 1u) * sizeof(*grown));
        if (!grown) return -1;
        resolver->out->zones = grown;
        char (*names)[AFX_C_SAMPLE_NAME_BYTES] = realloc(resolver->out->zone_names,
                                                          (size_t)(zone + 1u) * sizeof(*names));
        if (!names) return -1;
        resolver->out->zone_names = names;
        resolver->out->zones[zone] = candidate;
        copy_sample_name(resolver->out->zone_names[zone], resolver->font,
                         (uint32_t)controls->sample);
        ++resolver->out->zone_count;
    }
    afx_c_note_t *grown = realloc(resolver->out->notes,
                                  (size_t)(resolver->out->note_count + 1u) * sizeof(*grown));
    if (!grown) return -1;
    resolver->out->notes = grown;
    resolver->out->notes[resolver->out->note_count] = *resolver->source;
    resolver->out->notes[resolver->out->note_count].setup_index = (uint16_t)(zone + 1u);
    resolver->out->notes[resolver->out->note_count++].mix = sf2_mix(&selected, resolver->source,
                                                                      resolver->options);
    resolver->matched = 1;
    return 0;
}

static int resolve_instrument(resolver_t *resolver, uint32_t index, controls_t inherited,
                              modulators_t preset_modulators) {
    const sf2_t *font = resolver->font;
    if (index + 1u >= font->inst_count) return -1;
    uint32_t first = le16(font->inst + index * INST_BYTES + 20),
             last = le16(font->inst + (index + 1u) * INST_BYTES + 20);
    if (first > last || last >= font->ibag_count) return -1;
    controls_t global = controls_default();
    modulators_t global_modulators = {.velocity_attenuation_amount = 960};
    for (uint32_t bag = first; bag < last; ++bag) {
        uint32_t gens_first = le16(font->ibag + bag * BAG_BYTES),
                 gens_last = le16(font->ibag + (bag + 1u) * BAG_BYTES);
        controls_t local = read_controls(font->igen, font->igen_count, gens_first, gens_last);
        if (local.key_lo > local.key_hi || local.velocity_lo > local.velocity_hi) return -1;
        if (local.sample < 0) {
            global = overlay(global, &local);
            if (modulator_override(font->imod, font->imod_count, font->ibag, bag, font->ibag_count,
                                   &global_modulators)) return -1;
            continue;
        }
        controls_t instrument = overlay(global, &local);
        modulators_t instrument_modulators = global_modulators;
        if (modulator_override(font->imod, font->imod_count, font->ibag, bag, font->ibag_count,
                               &instrument_modulators)) return -1;
        controls_t combined = add_controls(inherited, &instrument);
        combined.velocity_attenuation_amount = preset_modulators.velocity_attenuation_amount +
                                               instrument_modulators.velocity_attenuation_amount;
        combined.velocity_filter_amount = preset_modulators.velocity_filter_amount +
                                          instrument_modulators.velocity_filter_amount;
        modulators_t all_modulators = preset_modulators;
        if (modulator_append(&all_modulators, &instrument_modulators)) return -1;
        if (combined.sample >= 0 && matches(&combined, resolver->source) &&
            append_note(resolver, &combined, &all_modulators)) return -1;
    }
    return 0;
}

static int resolve_preset(resolver_t *resolver, uint32_t index) {
    const sf2_t *font = resolver->font;
    if (index + 1u >= font->phdr_count) return -1;
    uint32_t first = le16(font->phdr + index * PHDR_BYTES + 24),
             last = le16(font->phdr + (index + 1u) * PHDR_BYTES + 24);
    if (first > last || last >= font->pbag_count) return -1;
    controls_t global = controls_default();
    modulators_t global_modulators = {0};
    for (uint32_t bag = first; bag < last; ++bag) {
        uint32_t gens_first = le16(font->pbag + bag * BAG_BYTES),
                 gens_last = le16(font->pbag + (bag + 1u) * BAG_BYTES);
        controls_t local = read_controls(font->pgen, font->pgen_count, gens_first, gens_last);
        if (local.key_lo > local.key_hi || local.velocity_lo > local.velocity_hi) return -1;
        if (local.instrument < 0) {
            global = overlay(global, &local);
            if (modulator_override(font->pmod, font->pmod_count, font->pbag, bag, font->pbag_count,
                                   &global_modulators)) return -1;
            continue;
        }
        controls_t combined = overlay(global, &local);
        modulators_t preset_modulators = global_modulators;
        if (modulator_override(font->pmod, font->pmod_count, font->pbag, bag, font->pbag_count,
                               &preset_modulators)) return -1;
        if (matches(&combined, resolver->source) &&
            resolve_instrument(resolver, (uint32_t)combined.instrument, combined, preset_modulators))
            return -1;
    }
    return 0;
}

void afx_c_sf2_output_free(afx_c_sf2_output_t *out) {
    if (!out) return;
    for (uint32_t i = 0; i < out->owned_count; ++i) free(out->owned_samples[i]);
    free(out->owned_samples); free(out->zone_names); free(out->zones); free(out->notes);
    *out = (afx_c_sf2_output_t){0};
}

void afx_c_sf2_options_default(afx_c_sf2_options_t *out) {
    if (!out) return;
    *out = (afx_c_sf2_options_t){.sample_format = AFX_SAMPLE_AUTO,
                                 .channel = AFX_C_SF2_STEREO,
                                 .filter_model = AFX_C_SF2_FILTER_ENVELOPE,
                                 .gain_model = AFX_C_SF2_GAIN_STANDARD,
                                 .envelope_model = AFX_C_SF2_ENVELOPE_SF2,
                                 .source_pan = 1, .source_modulators = 1};
}

int afx_c_sf2_resolve(const char *path, const afx_c_note_t *notes, uint32_t count,
                      const afx_c_sf2_options_t *options, afx_c_sf2_output_t *out) {
    uint8_t *data = NULL;
    uint32_t bytes = 0;
    sf2_t font;
    decoded_t *decoded = NULL;
    if (!path || !notes || !count || !options || !out || options->sample_format > AFX_SAMPLE_AUTO ||
        options->channel > AFX_C_SF2_RIGHT || options->filter_offset_cents < -16000 || options->filter_offset_cents > 16000 ||
        options->filter_model > AFX_C_SF2_FILTER_NONE || options->gain_model > AFX_C_SF2_GAIN_FLUIDSYNTH2 ||
        options->envelope_model > AFX_C_SF2_ENVELOPE_FIXED || options->gain_bias_centibels < -10200 || options->gain_bias_centibels > 10200 ||
        read_file(path, &data, &bytes) || load_font(data, bytes, &font)) goto failed;
    *out = (afx_c_sf2_output_t){0};
    if (font.shdr_count - 1u > UINT32_MAX / 2u) goto failed;
    decoded = calloc((font.shdr_count - 1u) * 2u, sizeof(*decoded));
    if (!decoded) goto failed;
    for (uint32_t note = 0; note < count; ++note) {
        uint32_t target_bank = (uint32_t)notes[note].bank_msb * 128u + notes[note].bank_lsb;
        int preset = -1;
        for (uint32_t index = 0; index + 1u < font.phdr_count; ++index)
            if (le16(font.phdr + index * PHDR_BYTES + 20) == notes[note].program &&
                le16(font.phdr + index * PHDR_BYTES + 22) == target_bank) { preset = (int)index; break; }
        if (preset < 0) goto failed;
        resolver_t resolver = {out, decoded, font.shdr_count - 1u, &font, notes + note, options, 0};
        if (resolve_preset(&resolver, (uint32_t)preset) || !resolver.matched) goto failed;
    }
    out->owned_samples = calloc((font.shdr_count - 1u) * 2u, sizeof(*out->owned_samples));
    if (!out->owned_samples) goto failed;
    for (uint32_t i = 0; i < (font.shdr_count - 1u) * 2u; ++i)
        if (decoded[i].ready) out->owned_samples[out->owned_count++] = decoded[i].data;
    free(decoded); free(data); return 0;
failed:
    if (decoded) for (uint32_t i = 0; i < (font.shdr_count - 1u) * 2u; ++i) free(decoded[i].data);
    free(decoded); free(data); afx_c_sf2_output_free(out); return -1;
}
