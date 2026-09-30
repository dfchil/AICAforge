#include "afx_compile_c.h"

#include <aicaflow/codec.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { AFB_HEADER = 32, AFX_HEADER = 80, RELOCATION_BYTES = 12, SINE_FRAMES = 128 };

typedef struct { uint32_t tick; uint8_t kind, channel, key, velocity; } event_t;

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
    return a->start_tick < b->start_tick ? -1 : a->start_tick > b->start_tick;
}

static int compare_event(const void *left, const void *right) {
    const event_t *a = left, *b = right;
    if (a->tick != b->tick) return a->tick < b->tick ? -1 : 1;
    return (int)a->kind - (int)b->kind; /* KEYOFF before NOTE on a shared tick. */
}

static uint16_t pitch(uint8_t key) {
    double ratio = pow(2.0, ((int)key - 69) / 12.0);
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

void afx_c_output_free(afx_c_output_t *out) {
    if (!out) return;
    free(out->afb); free(out->afx); *out = (afx_c_output_t){0};
}

int afx_c_compile_sine(const afx_c_note_t *input, uint32_t count,
                       uint32_t tick_rate, afx_c_output_t *out) {
    if (!out || !input || !count || !tick_rate) return -1;
    *out = (afx_c_output_t){0};
    afx_c_note_t *notes = malloc(count * sizeof(*notes));
    event_t *events = malloc(2u * count * sizeof(*events));
    /* A note can need a WAIT32 before both its KEYOFF and its NOTE. */
    uint8_t *stream = malloc(20u * count + 1u);
    if (!notes || !events || !stream) goto failed;
    memcpy(notes, input, count * sizeof(*notes)); qsort(notes, count, sizeof(*notes), compare_note);
    uint32_t ends[64] = {0}, channels = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!notes[i].velocity || notes[i].end_tick <= notes[i].start_tick) goto failed;
        uint32_t channel = 0;
        while (channel < channels && ends[channel] > notes[i].start_tick) ++channel;
        if (channel == channels) { if (channels == 64) goto failed; ++channels; }
        ends[channel] = notes[i].end_tick;
        events[2 * i] = (event_t){notes[i].start_tick, 1, (uint8_t)channel, notes[i].key, notes[i].velocity};
        events[2 * i + 1] = (event_t){notes[i].end_tick, 0, (uint8_t)channel, 0, 0};
    }
    qsort(events, 2u * count, sizeof(*events), compare_event);
    uint32_t cursor = 0, previous = 0;
    for (uint32_t i = 0; i < 2u * count; ++i) {
        cursor += encode_wait(stream + cursor, events[i].tick - previous); previous = events[i].tick;
        if (!events[i].kind) { stream[cursor++] = AFX_OP_KEYOFF; stream[cursor++] = events[i].channel; }
        else {
            stream[cursor++] = AFX_OP_NOTE_PL; stream[cursor++] = events[i].channel;
            afx_write16(stream + cursor, 0); cursor += 2;
            afx_write16(stream + cursor, pitch(events[i].key)); cursor += 2;
            afx_write16(stream + cursor, level(events[i].velocity)); cursor += 2;
        }
    }
    stream[cursor++] = AFX_OP_END;
    const uint32_t sample_bytes = SINE_FRAMES * 2u;
    out->afb_bytes = AFB_HEADER + sample_bytes;
    out->afb = calloc(1, out->afb_bytes);
    if (!out->afb) goto failed;
    for (uint32_t i = 0; i < SINE_FRAMES; ++i) {
        int16_t value = (int16_t)(sin(6.28318530717958647692 * i / SINE_FRAMES) * 28000.0);
        afx_write16(out->afb + AFB_HEADER + 2 * i, (uint16_t)value);
    }
    uint32_t bank_id = hash32(out->afb + AFB_HEADER, sample_bytes);
    afx_write32(out->afb, 0x00424641u); afx_write32(out->afb + 4, 1);
    afx_write32(out->afb + 8, bank_id); afx_write32(out->afb + 12, hash32_alt(out->afb + AFB_HEADER, sample_bytes));
    afx_write32(out->afb + 16, AFB_HEADER); afx_write32(out->afb + 20, sample_bytes);
    afx_write32(out->afb + 24, out->afb_bytes);
    uint32_t image_at = 96, image_bytes = AFX_SETUP_BYTES + cursor;
    out->afx_bytes = image_at + image_bytes;
    out->afx = calloc(1, out->afx_bytes);
    if (!out->afx) goto failed;
    uint8_t *afx = out->afx, *setup = afx + image_at;
    afx_write16(setup, 0x0200); afx_write16(setup + 2, 0);
    afx_write16(setup + 6, SINE_FRAMES); afx_write16(setup + 8, 0x001f); afx_write16(setup + 10, 0x001f);
    afx_write16(setup + 18, 0x0010); afx_write16(setup + 20, 0x0024);
    for (uint32_t i = 11; i < 16; ++i) afx_write16(setup + 2 * i, 0x1fffu);
    memcpy(setup + AFX_SETUP_BYTES, stream, cursor);
    uint32_t control_id = hash32(setup, image_bytes);
    afx_write32(afx, AFX_FILE_MAGIC); afx_write32(afx + 4, AFX_FILE_VERSION); afx_write32(afx + 8, out->afx_bytes);
    afx_write32(afx + 12, AFX_FLAG_MUSIC); afx_write32(afx + 16, image_at); afx_write32(afx + 20, image_bytes);
    afx_write32(afx + 24, AFX_SETUP_BYTES); afx_write32(afx + 28, cursor); afx_write32(afx + 32, control_id);
    afx_write32(afx + 36, 1); afx_write32(afx + 40, bank_id); afx_write32(afx + 44, afx_read32(out->afb + 12));
    afx_write32(afx + 48, AFX_HEADER); afx_write32(afx + 52, 1); afx_write32(afx + 64, channels);
    afx_write32(afx + 68, tick_rate); afx_write32(afx + 72, 1);
    afx_write32(afx + AFX_HEADER, 0); afx_write32(afx + AFX_HEADER + 4, 0); afx_write32(afx + AFX_HEADER + 8, sample_bytes);
    free(notes); free(events); free(stream); return 0;
failed:
    free(notes); free(events); free(stream); afx_c_output_free(out); return -1;
}
