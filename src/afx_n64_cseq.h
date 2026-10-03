#ifndef AICAFLOW_N64_CSEQ_H
#define AICAFLOW_N64_CSEQ_H

#include "afx_compile_c.h"

enum {
    AFX_N64_EVENT_PROGRAM = 1,
    AFX_N64_EVENT_CONTROL,
    AFX_N64_EVENT_PITCH
};

/* A CSeq event that can alter an already-running voice. It is authoring-only:
 * afx_n64 lowers it to ordinary PATCH instructions before the file reaches
 * Dreamcast. */
typedef struct {
    uint32_t tick, source_tick, source_track, source_order;
    uint8_t kind, channel, control, value;
    int16_t pitch_bend;
} afx_n64_automation_t;

/* Read one S1 ALSeqFile entry (or a bare CSeq) directly into the common
 * authoring timeline. The result is ordinary timed notes, never a generated
 * MIDI file. The caller owns `*out_notes`. */
int afx_c_n64_cseq_notes(const uint8_t *sequence, uint32_t bytes, int sequence_index,
                         uint32_t tick_rate, afx_c_note_t **out_notes,
                         uint32_t *out_count, afx_n64_automation_t **out_automation,
                         uint32_t *out_automation_count, uint32_t *out_duration);

#endif
