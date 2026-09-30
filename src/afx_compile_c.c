#include "afx_compile_c.h"

#include <aicaflow/codec.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { AFB_HEADER = 32, AFX_HEADER = 80, RELOCATION_BYTES = 12, SINE_FRAMES = 128 };

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

static uint16_t pitch(uint8_t key, uint8_t root_key) {
    double ratio = pow(2.0, ((int)key - root_key) / 12.0);
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

static int valid_sample(const afx_c_pcm16_t *sample) {
    return sample && sample->pcm16 && sample->frames && sample->frames <= 65535 &&
           sample->loop_start <= sample->loop_end && sample->loop_end < sample->frames;
}

void afx_c_output_free(afx_c_output_t *out) {
    if (!out) return;
    free(out->afb); free(out->afx); *out = (afx_c_output_t){0};
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
        if (zones[i].key_min > zones[i].key_max || !valid_sample(&zones[i].sample)) goto failed;
    memcpy(notes, input, count * sizeof(*notes)); qsort(notes, count, sizeof(*notes), compare_note);
    uint32_t ends[64] = {0}, channels = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!notes[i].velocity || notes[i].end_tick <= notes[i].start_tick) goto failed;
        uint32_t setup = zone_count;
        for (uint32_t scan = 0; scan < zone_count; ++scan)
            if (zones[scan].key_min <= notes[i].key && notes[i].key <= zones[scan].key_max) {
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
            afx_write16(stream + cursor, pitch(events[i].key, zones[events[i].setup].sample.root_key)); cursor += 2;
            afx_write16(stream + cursor, level(events[i].velocity)); cursor += 2;
        }
    }
    stream[cursor++] = AFX_OP_END;
    uint32_t sample_bytes = 0;
    for (uint32_t i = 0; i < zone_count; ++i) {
        sample_bytes = align32(sample_bytes);
        offsets[i] = sample_bytes;
        uint32_t bytes = zones[i].sample.frames * 2u;
        if (bytes > UINT32_MAX - sample_bytes) goto failed;
        sample_bytes += bytes;
    }
    if (sample_bytes > UINT32_MAX - AFB_HEADER) goto failed;
    out->afb_bytes = AFB_HEADER + sample_bytes;
    out->afb = calloc(1, out->afb_bytes);
    if (!out->afb) goto failed;
    for (uint32_t i = 0; i < zone_count; ++i)
        memcpy(out->afb + AFB_HEADER + offsets[i], zones[i].sample.pcm16, zones[i].sample.frames * 2u);
    uint32_t bank_id = hash32(out->afb + AFB_HEADER, sample_bytes);
    afx_write32(out->afb, 0x00424641u); afx_write32(out->afb + 4, 1);
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
        afx_write16(state, (zones[i].sample.loop ? 0x0200 : 0) | (address >> 16));
        afx_write16(state + 2, address);
        afx_write16(state + 4, zones[i].sample.loop_start); afx_write16(state + 6, zones[i].sample.loop_end);
        afx_write16(state + 8, 0x001f); afx_write16(state + 10, 0x001f);
        afx_write16(state + 18, 0x0010); afx_write16(state + 20, 0x0024);
        for (uint32_t field = 11; field < 16; ++field) afx_write16(state + 2 * field, 0x1fffu);
        uint8_t *relocation = afx + AFX_HEADER + i * RELOCATION_BYTES;
        afx_write32(relocation, i * AFX_SETUP_BYTES); afx_write32(relocation + 4, offsets[i]);
        afx_write32(relocation + 8, zones[i].sample.frames * 2u);
    }
    memcpy(setup + zone_count * AFX_SETUP_BYTES, stream, cursor);
    uint32_t control_id = hash32(setup, image_bytes);
    afx_write32(afx, AFX_FILE_MAGIC); afx_write32(afx + 4, AFX_FILE_VERSION); afx_write32(afx + 8, out->afx_bytes);
    afx_write32(afx + 12, AFX_FLAG_MUSIC); afx_write32(afx + 16, image_at); afx_write32(afx + 20, image_bytes);
    afx_write32(afx + 24, zone_count * AFX_SETUP_BYTES); afx_write32(afx + 28, cursor); afx_write32(afx + 32, control_id);
    afx_write32(afx + 36, zone_count); afx_write32(afx + 40, bank_id); afx_write32(afx + 44, afx_read32(out->afb + 12));
    afx_write32(afx + 48, AFX_HEADER); afx_write32(afx + 52, zone_count); afx_write32(afx + 64, channels);
    afx_write32(afx + 68, tick_rate); afx_write32(afx + 72, 1);
    free(notes); free(events); free(offsets); free(stream); return 0;
failed:
    free(notes); free(events); free(offsets); free(stream); afx_c_output_free(out); return -1;
}

int afx_c_compile_pcm16(const afx_c_note_t *notes, uint32_t count,
                        uint32_t tick_rate, const afx_c_pcm16_t *sample,
                        afx_c_output_t *out) {
    if (!sample) return -1;
    const afx_c_zone_t zone = {*sample, 0, 127};
    return afx_c_compile_zones(notes, count, tick_rate, &zone, 1, out);
}

int afx_c_compile_sine(const afx_c_note_t *notes, uint32_t count,
                       uint32_t tick_rate, afx_c_output_t *out) {
    uint8_t pcm[SINE_FRAMES * 2];
    for (uint32_t i = 0; i < SINE_FRAMES; ++i) {
        int16_t value = (int16_t)(sin(6.28318530717958647692 * i / SINE_FRAMES) * 28000.0);
        afx_write16(pcm + 2 * i, (uint16_t)value);
    }
    const afx_c_pcm16_t sample = {pcm, SINE_FRAMES, 69, 1, 0, SINE_FRAMES - 1};
    return afx_c_compile_pcm16(notes, count, tick_rate, &sample, out);
}
