#include "afx_midi_c.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t start, end;
    uint32_t source_id, track, order;
    uint8_t key, velocity, bank_msb, bank_lsb, program;
    uint8_t channel, volume, expression, pan;
    uint8_t controllers[128], poly_pressure, channel_pressure, pitch_sensitivity;
    int16_t pitch_bend;
    int32_t next_active, next_sustained;
} raw_note_t;
typedef struct { uint32_t tick, usec, order; } tempo_t;
typedef struct {
    uint32_t tick, order, value, track, track_order;
    uint8_t kind, channel, a, b;
} midi_event_t;

enum { EVENT_NOTE_OFF, EVENT_NOTE_ON, EVENT_CONTROL, EVENT_PROGRAM, EVENT_PITCH,
       EVENT_POLY_PRESSURE, EVENT_CHANNEL_PRESSURE, EVENT_TEMPO };

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int vlq(const uint8_t *data, uint32_t end, uint32_t *at, uint32_t *out) {
    uint32_t value = 0;
    for (uint32_t i = 0; i != 4; ++i) {
        if (*at == end || value > 0x0ffffffu) return -1;
        uint8_t byte = data[(*at)++]; value = value << 7 | (byte & 0x7fu);
        if (!(byte & 0x80u)) { *out = value; return 0; }
    }
    return -1;
}

static int grow(void **items, uint32_t *capacity, uint32_t size, uint32_t count) {
    if (count < *capacity) return 0;
    uint32_t next = *capacity ? *capacity * 2u : 32u;
    if (next < *capacity || next > UINT32_MAX / size) return -1;
    void *grown = realloc(*items, (size_t)next * size);
    if (!grown) return -1;
    *items = grown; *capacity = next; return 0;
}

static int compare_tempo(const void *left, const void *right) {
    const tempo_t *a = left, *b = right;
    if (a->tick != b->tick) return a->tick < b->tick ? -1 : 1;
    return a->order < b->order ? -1 : a->order > b->order;
}

static int event_priority(const midi_event_t *event) {
    if (event->kind == EVENT_TEMPO || event->kind == EVENT_CONTROL || event->kind == EVENT_PROGRAM || event->kind == EVENT_PITCH ||
        event->kind == EVENT_POLY_PRESSURE || event->kind == EVENT_CHANNEL_PRESSURE) return 0;
    return event->kind == EVENT_NOTE_OFF ? 1 : 2;
}

static int compare_event(const void *left, const void *right) {
    const midi_event_t *a = left, *b = right;
    if (a->tick != b->tick) return a->tick < b->tick ? -1 : 1;
    int ap = event_priority(a), bp = event_priority(b);
    if (ap != bp) return ap < bp ? -1 : 1;
    return a->order < b->order ? -1 : a->order > b->order;
}

static uint32_t control_tick(uint32_t source_tick, const tempo_t *tempos,
                             uint32_t tempo_count, uint32_t division, uint32_t rate) {
    uint64_t usec_times_division = 0, cursor = 0;
    uint32_t usec_per_beat = 500000;
    for (uint32_t i = 0; i < tempo_count && tempos[i].tick <= source_tick; ++i) {
        usec_times_division += (uint64_t)(tempos[i].tick - cursor) * usec_per_beat;
        cursor = tempos[i].tick; usec_per_beat = tempos[i].usec;
    }
    usec_times_division += (uint64_t)(source_tick - cursor) * usec_per_beat;
    uint64_t numerator = usec_times_division * rate;
    uint64_t denominator = (uint64_t)division * 1000000u;
    return (uint32_t)((2u * numerator + denominator) / (2u * denominator));
}

static void close_channel(raw_note_t *raw, int32_t active_head[16][128], int32_t active_tail[16][128],
                          uint8_t channel, uint32_t tick) {
    for (uint32_t key = 0; key < 128; ++key) {
        int32_t note = active_head[channel][key];
        while (note >= 0) { int32_t next = raw[note].next_active; raw[note].end = tick; note = next; }
        active_head[channel][key] = active_tail[channel][key] = -1;
    }
}

/* MIDI sustain is an input-timeline concern.  Bake it into note lifetimes so
 * AICAflow only needs ordinary NOTE/KEYOFF events at runtime. */
static void close_sustained(raw_note_t *raw, int32_t sustained[16],
                            uint8_t channel, uint32_t tick) {
    int32_t note = sustained[channel];
    while (note >= 0) {
        int32_t next = raw[note].next_sustained;
        raw[note].end = tick;
        raw[note].next_sustained = -1;
        note = next;
    }
    sustained[channel] = -1;
}

int afx_c_midi_notes(const void *input, uint32_t bytes, uint32_t tick_rate,
                     afx_c_note_t **out_notes, uint32_t *out_count) {
    const uint8_t *data = input;
    raw_note_t *raw = NULL; tempo_t *tempos = NULL; midi_event_t *events = NULL;
    uint32_t raw_count = 0, raw_capacity = 0, tempo_count = 0, tempo_capacity = 0;
    uint32_t event_count = 0, event_capacity = 0;
    uint32_t at = 0, format, tracks, division, event_order = 0, final_tick = 0;
    if (!out_notes || !out_count || !data || !tick_rate || bytes < 14 ||
        memcmp(data, "MThd", 4) || be32(data + 4) != 6) return -1;
    format = be16(data + 8); tracks = be16(data + 10); division = be16(data + 12); at = 14;
    if (format > 1 || !tracks || !division || (division & 0x8000u)) return -1;
    *out_notes = NULL; *out_count = 0;
    for (uint32_t track = 0; track < tracks; ++track) {
        uint32_t end, tick = 0, track_order = 0; uint8_t running = 0;
        if (at > bytes - 8 || memcmp(data + at, "MTrk", 4)) goto failed;
        uint32_t length = be32(data + at + 4); at += 8;
        if (length > bytes - at) goto failed;
        end = at + length;
        while (at < end) {
            uint32_t order = track_order++;
            uint32_t delta; uint8_t status, first = 0; int have_first = 0;
            if (vlq(data, end, &at, &delta) || UINT32_MAX - tick < delta) goto failed;
            tick += delta;
            if (at == end) goto failed;
            first = data[at];
            if (first & 0x80u) { status = first; ++at; }
            else { if (!running) goto failed; status = running; have_first = 1; ++at; }
            if (status < 0xf0u) {
                uint8_t kind = status & 0xf0u, channel = status & 0x0fu, a, b = 0;
                uint32_t need = kind == 0xc0u || kind == 0xd0u ? 1 : 2;
                running = status;
                if (have_first) a = first;
                else { if (at == end) goto failed; a = data[at++]; }
                if (need == 2) { if (at == end) goto failed; b = data[at++]; }
                if (a & 0x80u || b & 0x80u) goto failed;
                uint8_t event_kind;
                if (kind == 0x90u && b) event_kind = EVENT_NOTE_ON;
                else if (kind == 0x80u || (kind == 0x90u && !b)) event_kind = EVENT_NOTE_OFF;
                else if (kind == 0xa0u) event_kind = EVENT_POLY_PRESSURE;
                else if (kind == 0xb0u) event_kind = EVENT_CONTROL;
                else if (kind == 0xc0u) event_kind = EVENT_PROGRAM;
                else if (kind == 0xd0u) event_kind = EVENT_CHANNEL_PRESSURE;
                else if (kind == 0xe0u) event_kind = EVENT_PITCH;
                else continue;
                if (grow((void **)&events, &event_capacity, sizeof(*events), event_count)) goto failed;
                events[event_count++] = (midi_event_t){tick, event_order++, 0, track, order, event_kind, channel, a, b};
                continue;
            }
            running = 0;
            if (status == 0xffu) {
                uint32_t length;
                if (at == end) goto failed;
                uint8_t type = data[at++];
                if (vlq(data, end, &at, &length) || length > end - at) goto failed;
                if (type == 0x51u && length == 3) {
                    uint32_t usec = (uint32_t)data[at] << 16 | (uint32_t)data[at + 1] << 8 | data[at + 2];
                    if (!usec) goto failed;
                    if (grow((void **)&events, &event_capacity, sizeof(*events), event_count)) goto failed;
                    events[event_count++] = (midi_event_t){tick, event_order++, usec, track, order, EVENT_TEMPO, 0, 0, 0};
                }
                at += length;
            } else if (status == 0xf0u || status == 0xf7u) {
                uint32_t length;
                if (vlq(data, end, &at, &length) || length > end - at) goto failed;
                at += length;
            } else goto failed;
        }
        if (tick > final_tick) final_tick = tick;
    }
    if (at != bytes || !event_count) goto failed;
    qsort(events, event_count, sizeof(*events), compare_event);
    int32_t active_head[16][128], active_tail[16][128], sustained[16];
    uint8_t bank_msb[16] = {0}, bank_lsb[16] = {0}, program[16] = {0}, pedal[16] = {0};
    uint8_t volume[16], expression[16], pan[16], controllers[16][128], poly_pressure[16][128], channel_pressure[16],
            pitch_sensitivity[16], rpn_msb[16], rpn_lsb[16]; int16_t pitch_bend[16] = {0};
    memset(volume, 127, sizeof(volume)); memset(expression, 127, sizeof(expression));
    memset(pan, 64, sizeof(pan));
    memset(controllers, 0, sizeof(controllers)); memset(poly_pressure, 0, sizeof(poly_pressure));
    memset(channel_pressure, 0, sizeof(channel_pressure)); memset(pitch_sensitivity, 2, sizeof(pitch_sensitivity));
    memset(rpn_msb, 127, sizeof(rpn_msb)); memset(rpn_lsb, 127, sizeof(rpn_lsb));
    for (uint32_t channel = 0; channel < 16; ++channel) {
        controllers[channel][7] = 127; controllers[channel][10] = 64; controllers[channel][11] = 127;
    }
    memset(active_head, 0xff, sizeof(active_head)); memset(active_tail, 0xff, sizeof(active_tail));
    memset(sustained, 0xff, sizeof(sustained));
    for (uint32_t i = 0; i < event_count; ++i) {
        const midi_event_t *event = events + i;
        uint8_t channel = event->channel, key = event->a;
        if (event->kind == EVENT_TEMPO) {
            if (grow((void **)&tempos, &tempo_capacity, sizeof(*tempos), tempo_count)) goto failed;
            tempos[tempo_count++] = (tempo_t){event->tick, event->value, event->order};
        } else if (event->kind == EVENT_PROGRAM) {
            program[channel] = key;
        } else if (event->kind == EVENT_PITCH) {
            pitch_bend[channel] = (int16_t)(((unsigned)event->b << 7 | key) - 8192);
        } else if (event->kind == EVENT_POLY_PRESSURE) {
            poly_pressure[channel][key] = event->b;
        } else if (event->kind == EVENT_CHANNEL_PRESSURE) {
            channel_pressure[channel] = key;
        } else if (event->kind == EVENT_CONTROL) {
            controllers[channel][key] = event->b;
            if (key == 0u) bank_msb[channel] = event->b;
            else if (key == 32u) bank_lsb[channel] = event->b;
            else if (key == 7u) volume[channel] = event->b;
            else if (key == 10u) pan[channel] = event->b;
            else if (key == 11u) expression[channel] = event->b;
            else if (key == 101u) rpn_msb[channel] = event->b;
            else if (key == 100u) rpn_lsb[channel] = event->b;
            else if (key == 6u && rpn_msb[channel] == 0 && rpn_lsb[channel] == 0) pitch_sensitivity[channel] = event->b;
            else if (key == 64u) {
                if (event->b >= 64u) pedal[channel] = 1;
                else if (pedal[channel]) {
                    pedal[channel] = 0;
                    close_sustained(raw, sustained, channel, event->tick);
                }
            } else if (key == 120u || key == 123u) {
                close_channel(raw, active_head, active_tail, channel, event->tick);
                close_sustained(raw, sustained, channel, event->tick);
            }
        } else if (event->kind == EVENT_NOTE_ON) {
            if (grow((void **)&raw, &raw_capacity, sizeof(*raw), raw_count)) goto failed;
            raw[raw_count] = (raw_note_t){event->tick, 0, raw_count, event->track, event->track_order,
                                           key, event->b, bank_msb[channel], bank_lsb[channel], program[channel], channel, volume[channel],
                                           expression[channel], pan[channel], {0}, poly_pressure[channel][key], channel_pressure[channel],
                                           pitch_sensitivity[channel], pitch_bend[channel], -1, -1};
            memcpy(raw[raw_count].controllers, controllers[channel], sizeof(raw[raw_count].controllers));
            int32_t note = (int32_t)raw_count++;
            if (active_tail[channel][key] >= 0) raw[active_tail[channel][key]].next_active = note;
            else active_head[channel][key] = note;
            active_tail[channel][key] = note;
        } else if (active_head[channel][key] >= 0) {
            int32_t note = active_head[channel][key];
            active_head[channel][key] = raw[note].next_active;
            if (active_head[channel][key] < 0) active_tail[channel][key] = -1;
            raw[note].next_active = -1;
            if (pedal[channel]) {
                raw[note].next_sustained = sustained[channel];
                sustained[channel] = note;
            } else raw[note].end = event->tick;
        }
    }
    for (uint32_t channel = 0; channel < 16; ++channel) {
        close_channel(raw, active_head, active_tail, channel, final_tick);
        close_sustained(raw, sustained, channel, final_tick);
    }
    if (!raw_count) goto failed;
    if (tempo_count > 1) qsort(tempos, tempo_count, sizeof(*tempos), compare_tempo);
    afx_c_note_t *result = calloc(raw_count, sizeof(*result));
    if (!result) goto failed;
    for (uint32_t i = 0; i < raw_count; ++i) {
        result[i] = (afx_c_note_t){.start_tick = control_tick(raw[i].start, tempos, tempo_count, division, tick_rate),
                                   .end_tick = control_tick(raw[i].end, tempos, tempo_count, division, tick_rate),
                                   .key = raw[i].key, .velocity = raw[i].velocity, .bank_msb = raw[i].bank_msb,
                                   .bank_lsb = raw[i].bank_lsb, .program = raw[i].program,
                                   .source_id = raw[i].source_id, .source_track = raw[i].track,
                                   .source_order = raw[i].order, .source_tick = raw[i].start};
        result[i].release_tick = result[i].end_tick;
        result[i].controller_state = 1ull << 39 | (uint64_t)raw[i].channel |
                                     (uint64_t)raw[i].volume << 4 | (uint64_t)raw[i].expression << 11 |
                                     (uint64_t)raw[i].pan << 18 | (uint64_t)(raw[i].pitch_bend + 8192) << 25;
        memcpy(result[i].controllers, raw[i].controllers, sizeof(result[i].controllers));
        result[i].poly_pressure = raw[i].poly_pressure;
        result[i].channel_pressure = raw[i].channel_pressure;
        result[i].pitch_sensitivity = raw[i].pitch_sensitivity;
        if (result[i].end_tick <= result[i].start_tick) { free(result); goto failed; }
    }
    free(events); free(raw); free(tempos); *out_notes = result; *out_count = raw_count; return 0;
failed:
    free(events); free(raw); free(tempos); return -1;
}
