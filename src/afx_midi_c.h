#ifndef AICAFLOW_MIDI_C_H
#define AICAFLOW_MIDI_C_H

#include "afx_compile_c.h"

/* Read MIDI notes, tempo, bank select and program changes into the resolved
 * authoring timeline. The caller frees *out_notes. */
int afx_c_midi_notes(const void *data, uint32_t bytes, uint32_t tick_rate,
                     afx_c_note_t **out_notes, uint32_t *out_count);

#endif
