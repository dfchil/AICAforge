#include "afx_sf2_c.h"
#include "afx_sample_c.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PHDR_BYTES = 38, BAG_BYTES = 4, GEN_BYTES = 4, INST_BYTES = 22,
    SHDR_BYTES = 46, GEN_INSTRUMENT = 41, GEN_KEY_RANGE = 43,
    GEN_VELOCITY_RANGE = 44, GEN_COARSE_TUNE = 51, GEN_FINE_TUNE = 52,
    GEN_SAMPLE_ID = 53, GEN_SAMPLE_MODES = 54, GEN_OVERRIDE_ROOT = 58
};

typedef struct {
    const uint8_t *smpl, *phdr, *pbag, *pgen, *inst, *ibag, *igen, *shdr;
    uint32_t smpl_frames, phdr_count, pbag_count, pgen_count, inst_count,
             ibag_count, igen_count, shdr_count;
} sf2_t;

typedef struct {
    int instrument, sample, root, mode;
    int key_lo, key_hi, velocity_lo, velocity_hi, coarse, fine;
} controls_t;

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
    uint8_t sample_format;
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
        find_chunk(pdta, pdta_bytes, "pgen", &font->pgen, &chunk_bytes) || chunk_bytes % GEN_BYTES ||
        find_chunk(pdta, pdta_bytes, "inst", &font->inst, &chunk_bytes) || chunk_bytes % INST_BYTES ||
        find_chunk(pdta, pdta_bytes, "ibag", &font->ibag, &chunk_bytes) || chunk_bytes % BAG_BYTES ||
        find_chunk(pdta, pdta_bytes, "igen", &font->igen, &chunk_bytes) || chunk_bytes % GEN_BYTES ||
        find_chunk(pdta, pdta_bytes, "shdr", &font->shdr, &chunk_bytes) || chunk_bytes % SHDR_BYTES) return -1;
    find_chunk(sdta, sdta_bytes, "smpl", &font->smpl, &chunk_bytes); font->smpl_frames = chunk_bytes / 2u;
    find_chunk(pdta, pdta_bytes, "phdr", &font->phdr, &chunk_bytes); font->phdr_count = chunk_bytes / PHDR_BYTES;
    find_chunk(pdta, pdta_bytes, "pbag", &font->pbag, &chunk_bytes); font->pbag_count = chunk_bytes / BAG_BYTES;
    find_chunk(pdta, pdta_bytes, "pgen", &font->pgen, &chunk_bytes); font->pgen_count = chunk_bytes / GEN_BYTES;
    find_chunk(pdta, pdta_bytes, "inst", &font->inst, &chunk_bytes); font->inst_count = chunk_bytes / INST_BYTES;
    find_chunk(pdta, pdta_bytes, "ibag", &font->ibag, &chunk_bytes); font->ibag_count = chunk_bytes / BAG_BYTES;
    find_chunk(pdta, pdta_bytes, "igen", &font->igen, &chunk_bytes); font->igen_count = chunk_bytes / GEN_BYTES;
    find_chunk(pdta, pdta_bytes, "shdr", &font->shdr, &chunk_bytes); font->shdr_count = chunk_bytes / SHDR_BYTES;
    return font->phdr_count > 1 && font->pbag_count > 1 && font->inst_count > 1 &&
           font->ibag_count > 1 && font->shdr_count > 1 ? 0 : -1;
}

static controls_t controls_default(void) {
    return (controls_t){-1, -1, -1, -1, 0, 127, 0, 127, 0, 0};
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

static int matches(const controls_t *controls, const afx_c_note_t *note) {
    return controls->key_lo <= note->key && note->key <= controls->key_hi &&
           controls->velocity_lo <= note->velocity && note->velocity <= controls->velocity_hi;
}

static int ensure_sample(resolver_t *resolver, const controls_t *controls, uint32_t sample_id,
                         afx_c_sample_t *out) {
    const sf2_t *font = resolver->font;
    if (sample_id >= resolver->decoded_count) return -1;
    decoded_t *decoded = resolver->decoded + sample_id;
    if (!decoded->ready) {
        const uint8_t *header = font->shdr + sample_id * SHDR_BYTES;
        uint32_t start = le32(header + 20), end = le32(header + 24), loop_start = le32(header + 28),
                 loop_end = le32(header + 32), rate = le32(header + 36);
        if (!rate || start >= end || end > font->smpl_frames || (le16(header + 44) & 0x8000u)) return -1;
        uint32_t source_frames = end - start, step = (source_frames + 65534u) / 65535u;
        uint32_t frames = (source_frames + step - 1u) / step;
        uint8_t *pcm16 = malloc((size_t)frames * 2u), *encoded = NULL, format = 0;
        uint32_t encoded_bytes = 0;
        if (!pcm16) return -1;
        for (uint32_t i = 0; i < frames; ++i) {
            const uint8_t *source = font->smpl + 2u * (start + i * step);
            pcm16[2u * i] = source[0]; pcm16[2u * i + 1u] = source[1];
        }
        uint32_t local_start = loop_start > start ? (loop_start - start) / step : 0;
        uint32_t local_end = loop_end > start ? (loop_end - start) / step : 0;
        int loop_capable = local_start < local_end && local_end <= frames;
        /* A source with a legal loop must not receive automatic ADPCM: a later
         * SF2 zone may turn that loop on, and AICA's predictor seam needs an
         * explicit capture test. */
        if (afx_c_encode_sample(pcm16, frames, loop_capable, resolver->sample_format,
                                &encoded, &encoded_bytes, &format)) {
            free(pcm16); return -1;
        }
        free(pcm16);
        int rate_tune = (int)lrint(1200.0 * log2(44100.0 * step / rate));
        int tuning = (int)(int8_t)header[41] + rate_tune;
        if (tuning < INT16_MIN || tuning > INT16_MAX) { free(encoded); return -1; }
        decoded->data = encoded;
        decoded->sample = (afx_c_sample_t){encoded, encoded_bytes, frames, format,
                                           header[40] <= 127 ? header[40] : 60,
                                           0, 0, (uint16_t)(frames - 1u),
                                           (int16_t)tuning, rate / step};
        decoded->loop_start = (uint16_t)local_start;
        decoded->loop_end = (uint16_t)(loop_capable ? local_end - 1u : 0);
        decoded->loop_capable = loop_capable;
        decoded->ready = 1;
    }
    *out = decoded->sample;
    if ((controls->mode & 1) && decoded->loop_capable) {
        out->loop = 1; out->loop_start = decoded->loop_start; out->loop_end = decoded->loop_end;
    }
    if (controls->root >= 0) out->root_key = (uint8_t)controls->root;
    int tuning = (int)out->tuning_cents + controls->coarse * 100 + controls->fine;
    if (tuning < INT16_MIN || tuning > INT16_MAX) return -1;
    out->tuning_cents = (int16_t)tuning;
    return 0;
}

static int same_zone(const afx_c_zone_t *zone, const afx_c_sample_t *sample) {
    return zone->sample.data == sample->data && zone->sample.root_key == sample->root_key &&
           zone->sample.loop == sample->loop && zone->sample.loop_start == sample->loop_start &&
           zone->sample.loop_end == sample->loop_end && zone->sample.tuning_cents == sample->tuning_cents;
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

static int append_note(resolver_t *resolver, const controls_t *controls) {
    afx_c_sample_t sample;
    if (ensure_sample(resolver, controls, (uint32_t)controls->sample, &sample)) return -1;
    uint32_t zone = 0;
    while (zone < resolver->out->zone_count && !same_zone(resolver->out->zones + zone, &sample)) ++zone;
    if (zone == resolver->out->zone_count) {
        if (zone == UINT16_MAX) return -1;
        afx_c_zone_t *grown = realloc(resolver->out->zones, (size_t)(zone + 1u) * sizeof(*grown));
        if (!grown) return -1;
        resolver->out->zones = grown;
        char (*names)[AFX_C_SAMPLE_NAME_BYTES] = realloc(resolver->out->zone_names,
                                                          (size_t)(zone + 1u) * sizeof(*names));
        if (!names) return -1;
        resolver->out->zone_names = names;
        resolver->out->zones[zone] = (afx_c_zone_t){sample, 0, 127, 0, 127,
                                                     resolver->source->bank_msb,
                                                     resolver->source->bank_lsb,
                                                     resolver->source->program, 0};
        copy_sample_name(resolver->out->zone_names[zone], resolver->font,
                         (uint32_t)controls->sample);
        ++resolver->out->zone_count;
    }
    afx_c_note_t *grown = realloc(resolver->out->notes,
                                  (size_t)(resolver->out->note_count + 1u) * sizeof(*grown));
    if (!grown) return -1;
    resolver->out->notes = grown;
    resolver->out->notes[resolver->out->note_count] = *resolver->source;
    resolver->out->notes[resolver->out->note_count++].setup_index = (uint16_t)(zone + 1u);
    resolver->matched = 1;
    return 0;
}

static int resolve_instrument(resolver_t *resolver, uint32_t index, controls_t inherited) {
    const sf2_t *font = resolver->font;
    if (index + 1u >= font->inst_count) return -1;
    uint32_t first = le16(font->inst + index * INST_BYTES + 20),
             last = le16(font->inst + (index + 1u) * INST_BYTES + 20);
    if (first > last || last >= font->ibag_count) return -1;
    controls_t global = controls_default();
    for (uint32_t bag = first; bag < last; ++bag) {
        uint32_t gens_first = le16(font->ibag + bag * BAG_BYTES),
                 gens_last = le16(font->ibag + (bag + 1u) * BAG_BYTES);
        controls_t local = read_controls(font->igen, font->igen_count, gens_first, gens_last);
        if (local.key_lo > local.key_hi || local.velocity_lo > local.velocity_hi) return -1;
        if (local.sample < 0) { global = combine(global, local); continue; }
        controls_t combined = combine(inherited, combine(global, local));
        if (combined.sample >= 0 && matches(&combined, resolver->source) && append_note(resolver, &combined)) return -1;
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
    for (uint32_t bag = first; bag < last; ++bag) {
        uint32_t gens_first = le16(font->pbag + bag * BAG_BYTES),
                 gens_last = le16(font->pbag + (bag + 1u) * BAG_BYTES);
        controls_t local = read_controls(font->pgen, font->pgen_count, gens_first, gens_last);
        if (local.key_lo > local.key_hi || local.velocity_lo > local.velocity_hi) return -1;
        if (local.instrument < 0) { global = combine(global, local); continue; }
        controls_t combined = combine(global, local);
        if (matches(&combined, resolver->source) && resolve_instrument(resolver, (uint32_t)combined.instrument, combined))
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

int afx_c_sf2_resolve(const char *path, const afx_c_note_t *notes, uint32_t count,
                      uint8_t sample_format, afx_c_sf2_output_t *out) {
    uint8_t *data = NULL;
    uint32_t bytes = 0;
    sf2_t font;
    decoded_t *decoded = NULL;
    if (!path || !notes || !count || !out || sample_format > AFX_SAMPLE_AUTO ||
        read_file(path, &data, &bytes) || load_font(data, bytes, &font)) goto failed;
    *out = (afx_c_sf2_output_t){0};
    decoded = calloc(font.shdr_count - 1u, sizeof(*decoded));
    if (!decoded) goto failed;
    for (uint32_t note = 0; note < count; ++note) {
        uint32_t target_bank = (uint32_t)notes[note].bank_msb * 128u + notes[note].bank_lsb;
        int preset = -1;
        for (uint32_t index = 0; index + 1u < font.phdr_count; ++index)
            if (le16(font.phdr + index * PHDR_BYTES + 20) == notes[note].program &&
                le16(font.phdr + index * PHDR_BYTES + 22) == target_bank) { preset = (int)index; break; }
        if (preset < 0) goto failed;
        resolver_t resolver = {out, decoded, font.shdr_count - 1u, &font, notes + note, sample_format, 0};
        if (resolve_preset(&resolver, (uint32_t)preset) || !resolver.matched) goto failed;
    }
    out->owned_samples = calloc(font.shdr_count - 1u, sizeof(*out->owned_samples));
    if (!out->owned_samples) goto failed;
    for (uint32_t i = 0; i + 1u < font.shdr_count; ++i)
        if (decoded[i].ready) out->owned_samples[out->owned_count++] = decoded[i].data;
    free(decoded); free(data); return 0;
failed:
    if (decoded) for (uint32_t i = 0; i + 1u < font.shdr_count; ++i) free(decoded[i].data);
    free(decoded); free(data); afx_c_sf2_output_free(out); return -1;
}
