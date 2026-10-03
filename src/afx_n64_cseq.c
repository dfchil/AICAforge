#include "afx_n64_cseq.h"

#include <stdlib.h>
#include <string.h>

enum { CSEQ_NOTE, CSEQ_NOTE_OFF, CSEQ_PROGRAM, CSEQ_CONTROL, CSEQ_PITCH, CSEQ_TEMPO };

typedef struct { const uint8_t *data; uint32_t bytes, pos, backup_pos, backup_left; } reader_t;
typedef struct {
    uint32_t tick, order, duration;
    uint8_t kind, channel, a, b, track;
    uint32_t value;
} source_event_t;
typedef struct { afx_c_note_t note; int32_t next; } note_node_t;
typedef struct { uint32_t site; uint8_t remaining; } loop_state_t;

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int grow(void **data, uint32_t *capacity, uint32_t count, size_t size) {
    if (count < *capacity) return 0;
    uint32_t next = *capacity ? *capacity * 2u : 64u;
    if (next <= count || (uint64_t)next * size > SIZE_MAX) return -1;
    void *grown = realloc(*data, (size_t)next * size);
    if (!grown) return -1;
    *data = grown; *capacity = next;
    return 0;
}

static int reader_byte(reader_t *reader, uint8_t *out) {
    if (reader->backup_left) {
        if (reader->backup_pos >= reader->bytes) return -1;
        *out = reader->data[reader->backup_pos++]; --reader->backup_left; return 0;
    }
    if (reader->pos >= reader->bytes) return -1;
    uint8_t value = reader->data[reader->pos++];
    if (value != 0xfe) { *out = value; return 0; }
    if (reader->pos >= reader->bytes) return -1;
    uint8_t high = reader->data[reader->pos++];
    if (high == 0xfe) { *out = value; return 0; }
    if (reader->pos + 2u > reader->bytes) return -1;
    uint32_t distance = (uint32_t)high << 8 | reader->data[reader->pos];
    uint32_t length = reader->data[reader->pos + 1u]; reader->pos += 2;
    if (!length || distance + 4u > reader->pos) return -1;
    uint32_t start = reader->pos - distance - 4u;
    if (start + length > reader->bytes) return -1;
    reader->backup_pos = start + 1u; reader->backup_left = length - 1u; *out = reader->data[start]; return 0;
}

static int reader_varlen(reader_t *reader, uint32_t *out) {
    uint8_t byte; uint32_t value = 0;
    for (unsigned count = 0; count < 5; ++count) {
        if (reader_byte(reader, &byte) || value > UINT32_MAX >> 7) return -1;
        value = value << 7 | (byte & 0x7fu);
        if (!(byte & 0x80u)) { *out = value; return 0; }
    }
    return -1;
}

static int append(source_event_t **events, uint32_t *count, uint32_t *capacity,
                  source_event_t event) {
    if (grow((void **)events, capacity, *count, sizeof(**events))) return -1;
    (*events)[(*count)++] = event; return 0;
}

static int compare_source_event(const void *left, const void *right) {
    const source_event_t *a = left, *b = right;
    if (a->tick != b->tick) return a->tick < b->tick ? -1 : 1;
    if (a->track != b->track) return a->track < b->track ? -1 : 1;
    return a->order < b->order ? -1 : a->order > b->order;
}

static int parse_track(const uint8_t *data, uint32_t bytes, uint32_t offset, uint8_t track,
                       source_event_t **events, uint32_t *count, uint32_t *capacity) {
    reader_t reader = {.data = data, .bytes = bytes, .pos = offset};
    uint32_t tick = 0, delta, order = 0; uint8_t status = 0;
    loop_state_t loops[64];
    uint32_t loop_count = 0;
    if (reader_varlen(&reader, &delta)) return -1;
    for (uint32_t steps = 0; steps < 1000000u; ++steps) {
        if (delta > UINT32_MAX - tick) return -1;
        tick += delta;
        uint8_t byte;
        if (reader_byte(&reader, &byte)) return -1;
        if (byte == 0xff) {
            uint8_t meta;
            if (reader_byte(&reader, &meta)) return -1;
            if (meta == 0x2f) return 0;
            if (meta == 0x51) {
                uint8_t a, b, c;
                if (reader_byte(&reader, &a) || reader_byte(&reader, &b) || reader_byte(&reader, &c)) return -1;
                uint32_t tempo = (uint32_t)a << 16 | (uint32_t)b << 8 | c;
                if (!tempo || append(events, count, capacity,
                    (source_event_t){.tick = tick, .order = order++, .track = track, .kind = CSEQ_TEMPO, .value = tempo})) return -1;
                status = 0;
            } else if (meta == 0x2e) {
                uint8_t discard;
                if (reader_byte(&reader, &discard) || reader_byte(&reader, &discard)) return -1;
                status = 0;
            } else if (meta == 0x2d) {
                /* libaudio's compact loop has a mutable repeat byte.  Keep
                 * that state while parsing, but stop at its infinite form:
                 * one finite AFX cannot encode an endless source track. */
                uint8_t initial, current, a, b, c, d;
                uint32_t site = reader.pos;
                if (reader.backup_left || reader_byte(&reader, &initial) || reader_byte(&reader, &current) ||
                    reader_byte(&reader, &a) || reader_byte(&reader, &b) ||
                    reader_byte(&reader, &c) || reader_byte(&reader, &d)) return -1;
                if (current == 0xff) return 0;
                uint32_t distance = (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | d;
                if (distance) {
                    if (distance > reader.pos - offset) return -1;
                    uint32_t index = 0;
                    while (index < loop_count && loops[index].site != site) ++index;
                    if (index == loop_count) {
                        if (loop_count == sizeof(loops) / sizeof(loops[0])) return -1;
                        loops[loop_count++] = (loop_state_t){site, current};
                    }
                    if (loops[index].remaining) {
                        --loops[index].remaining;
                        reader.pos -= distance;
                    } else loops[index].remaining = initial;
                }
                status = 0;
            }
        } else {
            uint8_t first, second = 0;
            if (byte & 0x80u) { status = byte; if (reader_byte(&reader, &first)) return -1; }
            else if (status) first = byte;
            else return -1;
            uint8_t kind = status & 0xf0u, channel = status & 15u;
            if (kind != 0xc0 && kind != 0xd0 && reader_byte(&reader, &second)) return -1;
            if (first > 127 || second > 127) return -1;
            if (kind == 0x90 && second) {
                uint32_t duration;
                if (reader_varlen(&reader, &duration) || append(events, count, capacity,
                    (source_event_t){.tick = tick, .order = order++, .duration = duration, .track = track,
                                     .kind = CSEQ_NOTE, .channel = channel, .a = first, .b = second}) ||
                    duration > UINT32_MAX - tick || append(events, count, capacity,
                    (source_event_t){.tick = tick + duration, .order = order++, .track = track,
                                     .kind = CSEQ_NOTE_OFF, .channel = channel, .a = first})) return -1;
            } else if (kind == 0x80 || (kind == 0x90 && !second)) {
                if (append(events, count, capacity, (source_event_t){.tick = tick, .order = order++, .track = track,
                    .kind = CSEQ_NOTE_OFF, .channel = channel, .a = first})) return -1;
            } else if (kind == 0xc0) {
                if (append(events, count, capacity, (source_event_t){.tick = tick, .order = order++, .track = track,
                    .kind = CSEQ_PROGRAM, .channel = channel, .a = first})) return -1;
            } else if (kind == 0xb0) {
                if (append(events, count, capacity, (source_event_t){.tick = tick, .order = order++, .track = track,
                    .kind = CSEQ_CONTROL, .channel = channel, .a = first, .b = second})) return -1;
            } else if (kind == 0xe0) {
                if (append(events, count, capacity, (source_event_t){.tick = tick, .order = order++, .track = track,
                    .kind = CSEQ_PITCH, .channel = channel, .a = first, .b = second})) return -1;
            } else if (kind != 0x80 && kind != 0xa0 && kind != 0xd0) return -1;
        }
        if (reader_varlen(&reader, &delta)) return -1;
    }
    return -1;
}

static uint32_t control_tick(uint32_t tick, const source_event_t *events, uint32_t count,
                             uint32_t division, uint32_t rate) {
    uint64_t elapsed = 0, denominator = (uint64_t)division * 1000000u;
    uint32_t tempo = 500000, cursor = 0;
    for (uint32_t i = 0; i < count; ++i) if (events[i].kind == CSEQ_TEMPO) {
        if (events[i].tick > tick) break;
        elapsed += (uint64_t)(events[i].tick - cursor) * tempo;
        cursor = events[i].tick; tempo = events[i].value;
    }
    elapsed += (uint64_t)(tick - cursor) * tempo;
    uint64_t scaled = elapsed * rate;
    return (uint32_t)((2u * scaled + denominator) / (2u * denominator));
}

int afx_c_n64_cseq_notes(const uint8_t *sequence, uint32_t bytes, int sequence_index,
                         uint32_t tick_rate, afx_c_note_t **out_notes,
                         uint32_t *out_count, afx_n64_automation_t **out_automation,
                         uint32_t *out_automation_count, uint32_t *out_duration) {
    const uint8_t *data = sequence; uint32_t length = bytes, division;
    source_event_t *events = NULL; note_node_t *nodes = NULL;
    afx_n64_automation_t *automation = NULL;
    uint32_t event_count = 0, event_capacity = 0, note_count = 0, note_capacity = 0, final_tick = 0;
    if (!sequence || !bytes || !tick_rate || !out_notes || !out_count || !out_automation ||
        !out_automation_count || !out_duration) return -1;
    *out_notes = NULL; *out_count = 0; *out_automation = NULL; *out_automation_count = 0;
    if (sequence_index >= 0) {
        if (bytes < 4 || data[0] != 'S' || data[1] != '1' || (uint32_t)sequence_index >= ((uint32_t)data[2] << 8 | data[3])) goto failed;
        uint32_t at = 4u + (uint32_t)sequence_index * 8u;
        if (at + 8u > bytes) goto failed;
        uint32_t offset = be32(data + at), size = be32(data + at + 4u);
        if (offset > bytes || size < 68u || size > bytes - offset) goto failed;
        data += offset; length = size;
    }
    if (length < 68u) goto failed;
    division = be32(data + 64);
    if (!division || division > 0x7fffu) goto failed;
    for (uint8_t track = 0; track < 16; ++track) {
        uint32_t offset = be32(data + 4u * track);
        if (!offset) continue;
        if (offset < 68u || offset >= length || parse_track(data, length, offset, track, &events, &event_count, &event_capacity)) goto failed;
    }
    if (!event_count) goto failed;
    qsort(events, event_count, sizeof(*events), compare_source_event);
    /* CSeq stores a note's synthetic duration event beside its NOTE-on, but
     * the reference timeline orders that NOTE-off at its later timestamp.
     * Re-number each track after this time sort so source identity matches the
     * direct compact-sequence semantics without writing an intermediate MIDI. */
    uint32_t track_order[16] = {0};
    for (uint32_t i = 0; i < event_count; ++i) events[i].order = track_order[events[i].track]++;
    uint8_t program[16] = {0}, bank_msb[16] = {0}, bank_lsb[16] = {0};
    uint8_t controllers[16][128] = {{0}}; int16_t bend[16] = {0};
    int32_t active_head[16][128], active_tail[16][128];
    memset(active_head, 0xff, sizeof(active_head)); memset(active_tail, 0xff, sizeof(active_tail));
    automation = calloc(event_count, sizeof(*automation));
    if (!automation) goto failed;
    uint32_t automation_count = 0;
    for (uint32_t i = 0; i < event_count; ++i) {
        source_event_t *event = events + i; if (event->tick > final_tick) final_tick = event->tick;
        uint8_t channel = event->channel;
        if (event->kind == CSEQ_PROGRAM) {
            program[channel] = event->a;
            automation[automation_count++] = (afx_n64_automation_t){
                .tick = control_tick(event->tick, events, event_count, division, tick_rate),
                .source_tick = event->tick, .source_track = event->track, .source_order = event->order,
                .kind = AFX_N64_EVENT_PROGRAM, .channel = channel, .value = event->a};
        }
        else if (event->kind == CSEQ_CONTROL) {
            controllers[channel][event->a] = event->b;
            if (event->a == 0) bank_msb[channel] = event->b;
            else if (event->a == 32) bank_lsb[channel] = event->b;
            automation[automation_count++] = (afx_n64_automation_t){
                .tick = control_tick(event->tick, events, event_count, division, tick_rate),
                .source_tick = event->tick, .source_track = event->track, .source_order = event->order,
                .kind = AFX_N64_EVENT_CONTROL, .channel = channel, .control = event->a, .value = event->b};
        } else if (event->kind == CSEQ_PITCH) {
            bend[channel] = (int16_t)(((uint16_t)event->b << 7 | event->a) - 8192);
            automation[automation_count++] = (afx_n64_automation_t){
                .tick = control_tick(event->tick, events, event_count, division, tick_rate),
                .source_tick = event->tick, .source_track = event->track, .source_order = event->order,
                .kind = AFX_N64_EVENT_PITCH, .channel = channel, .pitch_bend = bend[channel]};
        }
        else if (event->kind == CSEQ_NOTE) {
            if (grow((void **)&nodes, &note_capacity, note_count, sizeof(*nodes))) goto failed;
            note_node_t *node = nodes + note_count;
            *node = (note_node_t){.next = -1};
            afx_c_note_t *note = &node->note;
            *note = (afx_c_note_t){.start_tick = control_tick(event->tick, events, event_count, division, tick_rate),
                .key = event->a, .velocity = event->b,
                .bank_msb = bank_msb[channel], .bank_lsb = bank_lsb[channel], .program = program[channel],
                .source_id = note_count, .source_track = event->track, .source_order = event->order, .source_tick = event->tick};
            note->controller_state = 1ull << 39 | (uint64_t)channel | (uint64_t)(bend[channel] + 8192) << 25;
            memcpy(note->controllers, controllers[channel], sizeof(note->controllers)); note->pitch_sensitivity = 2;
            if (active_tail[channel][event->a] >= 0) nodes[active_tail[channel][event->a]].next = (int32_t)note_count;
            else active_head[channel][event->a] = (int32_t)note_count;
            active_tail[channel][event->a] = (int32_t)note_count++;
        } else if (event->kind == CSEQ_NOTE_OFF && active_head[channel][event->a] >= 0) {
            int32_t index = active_head[channel][event->a];
            active_head[channel][event->a] = nodes[index].next;
            if (active_head[channel][event->a] < 0) active_tail[channel][event->a] = -1;
            nodes[index].next = -1;
            nodes[index].note.end_tick = control_tick(event->tick, events, event_count, division, tick_rate);
            nodes[index].note.release_tick = nodes[index].note.end_tick;
        }
    }
    if (!note_count) goto failed;
    *out_duration = control_tick(final_tick, events, event_count, division, tick_rate);
    afx_c_note_t *notes = calloc(note_count, sizeof(*notes));
    if (!notes) goto failed;
    for (uint32_t i = 0; i < note_count; ++i) {
        if (!nodes[i].note.end_tick) nodes[i].note.end_tick = nodes[i].note.release_tick = *out_duration;
        if (nodes[i].note.end_tick <= nodes[i].note.start_tick) { free(notes); goto failed; }
        notes[i] = nodes[i].note;
    }
    *out_notes = notes; *out_count = note_count;
    *out_automation = automation; *out_automation_count = automation_count;
    free(events); free(nodes); return 0;
failed:
    free(events); free(nodes); free(automation); return -1;
}
