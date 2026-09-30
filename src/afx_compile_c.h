#ifndef AICAFLOW_COMPILE_C_H
#define AICAFLOW_COMPILE_C_H

#include <stdint.h>

typedef struct {
    uint32_t start_tick, end_tick;
    uint8_t key, velocity;
} afx_c_note_t;

typedef struct {
    uint8_t *afb, *afx;
    uint32_t afb_bytes, afx_bytes;
} afx_c_output_t;

/* Compile a resolved monophonic/polyphonic note timeline using the built-in
 * sine source. The caller owns out through afx_c_output_free(). */
int afx_c_compile_sine(const afx_c_note_t *notes, uint32_t count,
                       uint32_t tick_rate, afx_c_output_t *out);
void afx_c_output_free(afx_c_output_t *out);

#endif
