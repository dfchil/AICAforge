#ifndef AICAFLOW_N64_CSEQ_H
#define AICAFLOW_N64_CSEQ_H

#include "afx_compile_c.h"

/* Read one S1 ALSeqFile entry (or a bare CSeq) directly into the common
 * authoring timeline. The result is ordinary timed notes, never a generated
 * MIDI file. The caller owns `*out_notes`. */
int afx_c_n64_cseq_notes(const uint8_t *sequence, uint32_t bytes, int sequence_index,
                         uint32_t tick_rate, afx_c_note_t **out_notes,
                         uint32_t *out_count, uint32_t *out_duration);

#endif
