#include "afx_midi_c.h"

#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t start, end; uint8_t key, velocity; } raw_note_t;
typedef struct { uint32_t tick, usec, order; } tempo_t;

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

static void close_channel(raw_note_t *raw, int32_t active[16][128],
                          uint8_t channel, uint32_t tick) {
    for (uint32_t key = 0; key < 128; ++key) {
        if (active[channel][key] >= 0) {
            raw[active[channel][key]].end = tick;
            active[channel][key] = -1;
        }
    }
}

int afx_c_midi_notes(const void *input, uint32_t bytes, uint32_t tick_rate,
                     afx_c_note_t **out_notes, uint32_t *out_count) {
    const uint8_t *data = input;
    raw_note_t *raw = NULL; tempo_t *tempos = NULL;
    uint32_t raw_count = 0, raw_capacity = 0, tempo_count = 0, tempo_capacity = 0;
    uint32_t at = 0, format, tracks, division, tempo_order = 0;
    if (!out_notes || !out_count || !data || !tick_rate || bytes < 14 ||
        memcmp(data, "MThd", 4) || be32(data + 4) != 6) return -1;
    format = be16(data + 8); tracks = be16(data + 10); division = be16(data + 12); at = 14;
    if (format > 1 || !tracks || !division || (division & 0x8000u)) return -1;
    *out_notes = NULL; *out_count = 0;
    for (uint32_t track = 0; track < tracks; ++track) {
        int32_t active[16][128];
        uint32_t end, tick = 0; uint8_t running = 0;
        memset(active, 0xff, sizeof(active));
        if (at > bytes - 8 || memcmp(data + at, "MTrk", 4)) goto failed;
        uint32_t length = be32(data + at + 4); at += 8;
        if (length > bytes - at) goto failed;
        end = at + length;
        while (at < end) {
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
                if (kind == 0x90u && b) {
                    if (active[channel][a] >= 0) raw[active[channel][a]].end = tick;
                    if (grow((void **)&raw, &raw_capacity, sizeof(*raw), raw_count)) goto failed;
                    raw[raw_count] = (raw_note_t){tick, 0, a, b}; active[channel][a] = (int32_t)raw_count++;
                } else if (kind == 0x80u || (kind == 0x90u && !b)) {
                    if (active[channel][a] >= 0) {
                        raw[active[channel][a]].end = tick; active[channel][a] = -1;
                    }
                } else if (kind == 0xb0u && (a == 120u || a == 123u)) {
                    close_channel(raw, active, channel, tick);
                }
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
                    if (grow((void **)&tempos, &tempo_capacity, sizeof(*tempos), tempo_count)) goto failed;
                    tempos[tempo_count++] = (tempo_t){tick, usec, tempo_order++};
                }
                at += length;
            } else if (status == 0xf0u || status == 0xf7u) {
                uint32_t length;
                if (vlq(data, end, &at, &length) || length > end - at) goto failed;
                at += length;
            } else goto failed;
        }
        for (uint32_t channel = 0; channel < 16; ++channel)
            for (uint32_t key = 0; key < 128; ++key)
                if (active[channel][key] >= 0) goto failed;
    }
    if (at != bytes || !raw_count) goto failed;
    if (tempo_count > 1) qsort(tempos, tempo_count, sizeof(*tempos), compare_tempo);
    afx_c_note_t *result = calloc(raw_count, sizeof(*result));
    if (!result) goto failed;
    for (uint32_t i = 0; i < raw_count; ++i) {
        result[i] = (afx_c_note_t){control_tick(raw[i].start, tempos, tempo_count, division, tick_rate),
                                   control_tick(raw[i].end, tempos, tempo_count, division, tick_rate),
                                   raw[i].key, raw[i].velocity};
        if (result[i].end_tick <= result[i].start_tick) { free(result); goto failed; }
    }
    free(raw); free(tempos); *out_notes = result; *out_count = raw_count; return 0;
failed:
    free(raw); free(tempos); return -1;
}
