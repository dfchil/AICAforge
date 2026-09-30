#ifndef AICAFLOW_MIDI_C_H
#define AICAFLOW_MIDI_C_H

#include "afx_compile_c.h"

/* Read the note and tempo subset of a Standard MIDI file into the resolved
 * timeline consumed by afx_c_compile_sine().  The caller frees *out_notes. */
int afx_c_midi_notes(const void *data, uint32_t bytes, uint32_t tick_rate,
                     afx_c_note_t **out_notes, uint32_t *out_count);

#endif
