#include "afx_compile_c.h"

#include <aicaflow/codec.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { AFB_HEADER = AFX_BANK_HEADER_BYTES, AFC_HEADER = AFX_SEEK_HEADER_BYTES,
       AFX_HEADER = 80, RELOCATION_BYTES = 12,
       SINE_FRAMES = 128, VISUAL_HEADER = 12, VISUAL_BANDS = 32, VISUAL_RATE = 60 };

typedef struct { uint32_t tick; uint8_t kind, channel, key, velocity; uint16_t setup; } event_t;

static uint32_t hash32(const uint8_t *p, uint32_t bytes) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 16777619u;
    return h ? h : 1;
}

static uint32_t hash32_alt(const uint8_t *p, uint32_t bytes) {
    uint32_t h = 2166136261u ^ 0x9e3779b9u;
    for (uint32_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 16777619u;
    return h ? h : 1;
}

static int compare_note(const void *left, const void *right) {
    const afx_c_note_t *a = left, *b = right;
    if (a->start_tick != b->start_tick) return a->start_tick < b->start_tick ? -1 : 1;
    if (a->end_tick != b->end_tick) return a->end_tick < b->end_tick ? -1 : 1;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return a->velocity < b->velocity ? -1 : a->velocity > b->velocity;
}

static int compare_event(const void *left, const void *right) {
    const event_t *a = left, *b = right;
    if (a->tick != b->tick) return a->tick < b->tick ? -1 : 1;
    if (a->kind != b->kind) return (int)a->kind - (int)b->kind; /* KEYOFF first. */
    return (int)a->channel - (int)b->channel;
}

static uint16_t pitch(uint8_t key, uint8_t root_key, int16_t tuning_cents) {
    double ratio = pow(2.0, (((int)key - root_key) * 100.0 - tuning_cents) / 1200.0);
    int octave = (int)floor(log2(ratio));
    int fraction = (int)(1024.0 * (ratio / pow(2.0, octave) - 1.0));
    if (octave < -8) octave = -8;
    if (octave > 7) octave = 7;
    if (fraction < 0) fraction = 0;
    if (fraction > 1023) fraction = 1023;
    return (uint16_t)(((octave & 15) << 11) | fraction);
}

static uint16_t level(uint8_t velocity) {
    uint32_t attenuation = (127u - velocity) * 2u;
    return (uint16_t)((attenuation > 255 ? 255 : attenuation) << 8 | 0x24u);
}

static uint32_t encode_wait(uint8_t *out, uint32_t ticks) {
    if (!ticks) return 0;
    if (ticks <= 255) { out[0] = AFX_OP_WAIT8; out[1] = (uint8_t)ticks; return 2; }
    if (ticks <= 65535) { out[0] = AFX_OP_WAIT16; afx_write16(out + 1, (uint16_t)ticks); return 3; }
    out[0] = AFX_OP_WAIT32; afx_write32(out + 1, ticks); return 5;
}

static uint32_t align32(uint32_t value) { return (value + 31u) & ~31u; }

static int valid_sample(const afx_c_sample_t *sample) {
    uint32_t minimum;
    if (!sample || !sample->data || !sample->frames || sample->frames > 65535 ||
        sample->format > AFX_ADPCM || sample->loop_start > sample->loop_end ||
        sample->loop_end >= sample->frames) return 0;
    minimum = sample->format == AFX_PCM16 ? sample->frames * 2u :
              sample->format == AFX_PCM8 ? sample->frames : (sample->frames + 1u) / 2u;
    return sample->bytes == minimum;
}

void afx_c_output_free(afx_c_output_t *out) {
    if (!out) return;
    free(out->afb); free(out->afx); free(out->afc); free(out->afv); *out = (afx_c_output_t){0};
}

static int build_seek(afx_c_output_t *out, uint32_t control_id, uint32_t bank_low,
                      uint32_t bank_high, uint32_t stream_offset) {
    /* ponytail: one tick-zero checkpoint keeps the first C implementation
     * correct but makes long-song seeks replay from the start. Add periodic
     * checkpoints when measured seeking needs it; the AFC ABI stays unchanged. */
    const uint32_t payload = 32;
    out->afc_bytes = AFC_HEADER + payload;
    out->afc = calloc(1, out->afc_bytes);
    if (!out->afc) return -1;
    afx_write32(out->afc, AFX_SEEK_MAGIC); afx_write32(out->afc + 4, AFX_SEEK_VERSION);
    afx_write32(out->afc + 8, control_id); afx_write32(out->afc + 12, bank_low);
    afx_write32(out->afc + 16, bank_high); afx_write32(out->afc + 20, AFC_HEADER);
    afx_write32(out->afc + 24, payload); afx_write32(out->afc + 28, out->afc_bytes);
    afx_write32(out->afc + AFC_HEADER, AFX_CHECKPOINT_MAGIC);
    afx_write32(out->afc + AFC_HEADER + 4, AFX_CHECKPOINT_VERSION);
    afx_write32(out->afc + AFC_HEADER + 8, 1);
    afx_write32(out->afc + AFC_HEADER + 16, 0);
    afx_write32(out->afc + AFC_HEADER + 20, stream_offset);
    return 0;
}

static int build_visual(const afx_c_note_t *notes, uint32_t count, uint32_t tick_rate,
                        afx_c_output_t *out) {
    uint32_t end = 0;
    for (uint32_t i = 0; i < count; ++i) if (notes[i].end_tick > end) end = notes[i].end_tick;
    uint64_t frames64 = ((uint64_t)end * VISUAL_RATE + tick_rate - 1u) / tick_rate;
    if (!frames64 || frames64 > (UINT32_MAX - VISUAL_HEADER) / VISUAL_BANDS) return -1;
    uint32_t frames = (uint32_t)frames64;
    out->afv_bytes = VISUAL_HEADER + frames * VISUAL_BANDS;
    out->afv = calloc(1, out->afv_bytes);
    if (!out->afv) return -1;
    memcpy(out->afv, "VIZ1", 4); out->afv[4] = 1; out->afv[5] = VISUAL_BANDS;
    out->afv[6] = VISUAL_RATE; afx_write32(out->afv + 8, frames);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        uint64_t tick = (uint64_t)frame * tick_rate / VISUAL_RATE;
        unsigned levels[VISUAL_BANDS] = {0};
        for (uint32_t i = 0; i < count; ++i) {
            if (tick < notes[i].start_tick || tick >= notes[i].end_tick) continue;
            double frequency = 440.0 * pow(2.0, ((int)notes[i].key - 69) / 12.0);
            double normalized = log(frequency / 80.0) / log(5000.0 / 80.0);
            int band = (int)floor(normalized * VISUAL_BANDS);
            if (band < 0) band = 0;
            if (band >= VISUAL_BANDS) band = VISUAL_BANDS - 1;
            levels[band] += notes[i].velocity;
        }
        for (uint32_t band = 0; band < VISUAL_BANDS; ++band)
            out->afv[VISUAL_HEADER + frame * VISUAL_BANDS + band] =
                (uint8_t)(levels[band] > 255 ? 255 : levels[band]);
    }
    return 0;
}

int afx_c_compile_zones(const afx_c_note_t *input, uint32_t count,
                        uint32_t tick_rate, const afx_c_zone_t *zones,
                        uint32_t zone_count, afx_c_output_t *out) {
    if (!out || !input || !count || !tick_rate || !zones || !zone_count || zone_count > UINT16_MAX)
        return -1;
    *out = (afx_c_output_t){0};
    afx_c_note_t *notes = malloc(count * sizeof(*notes));
    event_t *events = malloc(2u * count * sizeof(*events));
    uint32_t *offsets = malloc(zone_count * sizeof(*offsets));
    /* A note can need a WAIT32 before both its KEYOFF and its NOTE. */
    uint8_t *stream = malloc(20u * count + 1u);
    if (!notes || !events || !offsets || !stream) goto failed;
    for (uint32_t i = 0; i < zone_count; ++i)
        if (zones[i].key_min > zones[i].key_max ||
            zones[i].velocity_min > zones[i].velocity_max || !valid_sample(&zones[i].sample)) goto failed;
    memcpy(notes, input, count * sizeof(*notes)); qsort(notes, count, sizeof(*notes), compare_note);
    uint32_t ends[64] = {0}, channels = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!notes[i].velocity || notes[i].end_tick <= notes[i].start_tick) goto failed;
        uint32_t setup = zone_count;
        if (notes[i].setup_index) {
            if (notes[i].setup_index > zone_count) goto failed;
            setup = notes[i].setup_index - 1u;
        } else for (uint32_t scan = 0; scan < zone_count; ++scan)
            if (zones[scan].bank_msb == notes[i].bank_msb &&
                zones[scan].bank_lsb == notes[i].bank_lsb &&
                zones[scan].program == notes[i].program &&
                zones[scan].key_min <= notes[i].key && notes[i].key <= zones[scan].key_max) {
                if (notes[i].velocity < zones[scan].velocity_min ||
                    notes[i].velocity > zones[scan].velocity_max) continue;
                if (setup != zone_count) goto failed;
                setup = scan;
            }
        if (setup == zone_count) goto failed;
        uint32_t channel = 0;
        while (channel < channels && ends[channel] > notes[i].start_tick) ++channel;
        if (channel == channels) { if (channels == 64) goto failed; ++channels; }
        ends[channel] = notes[i].end_tick;
        events[2 * i] = (event_t){notes[i].start_tick, 1, (uint8_t)channel, notes[i].key, notes[i].velocity, (uint16_t)setup};
        events[2 * i + 1] = (event_t){notes[i].end_tick, 0, (uint8_t)channel, 0, 0, 0};
    }
    qsort(events, 2u * count, sizeof(*events), compare_event);
    uint32_t cursor = 0, previous = 0;
    for (uint32_t i = 0; i < 2u * count; ++i) {
        cursor += encode_wait(stream + cursor, events[i].tick - previous); previous = events[i].tick;
        if (!events[i].kind) { stream[cursor++] = AFX_OP_KEYOFF; stream[cursor++] = events[i].channel; }
        else {
            stream[cursor++] = AFX_OP_NOTE_PL; stream[cursor++] = events[i].channel;
            afx_write16(stream + cursor, events[i].setup); cursor += 2;
            afx_write16(stream + cursor, pitch(events[i].key, zones[events[i].setup].sample.root_key,
                                                zones[events[i].setup].sample.tuning_cents)); cursor += 2;
            afx_write16(stream + cursor, level(events[i].velocity)); cursor += 2;
        }
    }
    stream[cursor++] = AFX_OP_END;
    uint32_t sample_bytes = 0;
    for (uint32_t i = 0; i < zone_count; ++i) {
        for (uint32_t prior = 0; prior < i; ++prior)
            if (zones[i].sample.data == zones[prior].sample.data &&
                zones[i].sample.bytes == zones[prior].sample.bytes) {
                offsets[i] = offsets[prior];
                goto sample_known;
            }
        sample_bytes = align32(sample_bytes);
        offsets[i] = sample_bytes;
        uint32_t bytes = zones[i].sample.bytes;
        if (bytes > UINT32_MAX - sample_bytes) goto failed;
        sample_bytes += bytes;
sample_known:;
    }
    if (sample_bytes > UINT32_MAX - AFB_HEADER) goto failed;
    out->afb_bytes = AFB_HEADER + sample_bytes;
    out->afb = calloc(1, out->afb_bytes);
    if (!out->afb) goto failed;
    for (uint32_t i = 0; i < zone_count; ++i) {
        int first = 1;
        for (uint32_t prior = 0; prior < i; ++prior)
            if (offsets[prior] == offsets[i]) { first = 0; break; }
        if (first) memcpy(out->afb + AFB_HEADER + offsets[i], zones[i].sample.data, zones[i].sample.bytes);
    }
    uint32_t bank_id = hash32(out->afb + AFB_HEADER, sample_bytes);
    afx_write32(out->afb, AFX_BANK_MAGIC); afx_write32(out->afb + 4, AFX_BANK_VERSION);
    afx_write32(out->afb + 8, bank_id); afx_write32(out->afb + 12, hash32_alt(out->afb + AFB_HEADER, sample_bytes));
    afx_write32(out->afb + 16, AFB_HEADER); afx_write32(out->afb + 20, sample_bytes);
    afx_write32(out->afb + 24, out->afb_bytes);
    uint32_t image_at = align32(AFX_HEADER + zone_count * RELOCATION_BYTES);
    uint32_t image_bytes = zone_count * AFX_SETUP_BYTES + cursor;
    out->afx_bytes = image_at + image_bytes;
    out->afx = calloc(1, out->afx_bytes);
    if (!out->afx) goto failed;
    uint8_t *afx = out->afx, *setup = afx + image_at;
    for (uint32_t i = 0; i < zone_count; ++i) {
        uint8_t *state = setup + i * AFX_SETUP_BYTES;
        uint32_t address = offsets[i];
        if (address > 0x7fffffu) goto failed;
        afx_write16(state, (zones[i].sample.loop ? 0x0200 : 0) |
                    ((uint16_t)zones[i].sample.format << 7) | (address >> 16));
        afx_write16(state + 2, address);
        afx_write16(state + 4, zones[i].sample.loop_start); afx_write16(state + 6, zones[i].sample.loop_end);
        afx_write16(state + 8, 0x001f); afx_write16(state + 10, 0x001f);
        afx_write16(state + 16, zones[i].dsp_send);
        /* DISDL must be nonzero: 0x0010 has centre pan but mutes direct audio. */
        afx_write16(state + 18, 0x0f10); afx_write16(state + 20, 0x0024);
        for (uint32_t field = 11; field < 16; ++field) afx_write16(state + 2 * field, 0x1fffu);
        uint8_t *relocation = afx + AFX_HEADER + i * RELOCATION_BYTES;
        afx_write32(relocation, i * AFX_SETUP_BYTES); afx_write32(relocation + 4, offsets[i]);
        afx_write32(relocation + 8, zones[i].sample.bytes);
    }
    memcpy(setup + zone_count * AFX_SETUP_BYTES, stream, cursor);
    uint32_t control_id = afx_control_id(setup, image_bytes);
    afx_write32(afx, AFX_FILE_MAGIC); afx_write32(afx + 4, AFX_FILE_VERSION); afx_write32(afx + 8, out->afx_bytes);
    afx_write32(afx + 12, AFX_FLAG_MUSIC); afx_write32(afx + 16, image_at); afx_write32(afx + 20, image_bytes);
    afx_write32(afx + 24, zone_count * AFX_SETUP_BYTES); afx_write32(afx + 28, cursor); afx_write32(afx + 32, control_id);
    afx_write32(afx + 36, zone_count); afx_write32(afx + 40, bank_id); afx_write32(afx + 44, afx_read32(out->afb + 12));
    afx_write32(afx + 48, AFX_HEADER); afx_write32(afx + 52, zone_count); afx_write32(afx + 64, channels);
    afx_write32(afx + 68, tick_rate); afx_write32(afx + 72, 1);
    if (build_seek(out, control_id, bank_id, afx_read32(out->afb + 12), zone_count * AFX_SETUP_BYTES) ||
        build_visual(notes, count, tick_rate, out)) goto failed;
    free(notes); free(events); free(offsets); free(stream); return 0;
failed:
    free(notes); free(events); free(offsets); free(stream); afx_c_output_free(out); return -1;
}

int afx_c_compile_sample(const afx_c_note_t *notes, uint32_t count,
                         uint32_t tick_rate, const afx_c_sample_t *sample,
                         afx_c_output_t *out) {
    if (!sample) return -1;
    const afx_c_zone_t zone = {*sample, 0, 127, 0, 127, 0, 0, 0, 0};
    return afx_c_compile_zones(notes, count, tick_rate, &zone, 1, out);
}

int afx_c_compile_sine(const afx_c_note_t *notes, uint32_t count,
                       uint32_t tick_rate, afx_c_output_t *out) {
    uint8_t pcm[SINE_FRAMES * 2];
    for (uint32_t i = 0; i < SINE_FRAMES; ++i) {
        int16_t value = (int16_t)(sin(6.28318530717958647692 * i / SINE_FRAMES) * 28000.0);
        afx_write16(pcm + 2 * i, (uint16_t)value);
    }
    const afx_c_sample_t sample = {pcm, sizeof(pcm), SINE_FRAMES, AFX_PCM16, 69, 1,
                                   0, SINE_FRAMES - 1, 0, 44100};
    return afx_c_compile_sample(notes, count, tick_rate, &sample, out);
}
