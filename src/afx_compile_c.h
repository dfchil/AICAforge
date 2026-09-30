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

/* One explicit PCM16 source. `loop_end` is inclusive, as in AICA's LEA
 * register.  A non-looping source still has valid bounds but clears looping. */
typedef struct {
    const uint8_t *pcm16;
    uint32_t frames;
    uint8_t root_key, loop;
    uint16_t loop_start, loop_end;
} afx_c_pcm16_t;

typedef struct {
    afx_c_pcm16_t sample;
    uint8_t key_min, key_max;
} afx_c_zone_t;

/* Every note must select exactly one key range. The zones become the AFB's
 * contiguous, 32-byte aligned samples and the AFX setup dictionary. */
int afx_c_compile_zones(const afx_c_note_t *notes, uint32_t count,
                        uint32_t tick_rate, const afx_c_zone_t *zones,
                        uint32_t zone_count, afx_c_output_t *out);

/* Compile a resolved timeline against one explicit PCM16 sample. */
int afx_c_compile_pcm16(const afx_c_note_t *notes, uint32_t count,
                        uint32_t tick_rate, const afx_c_pcm16_t *sample,
                        afx_c_output_t *out);

/* Compile a resolved monophonic/polyphonic note timeline using the built-in
 * sine source. The caller owns out through afx_c_output_free(). */
int afx_c_compile_sine(const afx_c_note_t *notes, uint32_t count,
                       uint32_t tick_rate, afx_c_output_t *out);
void afx_c_output_free(afx_c_output_t *out);

#endif
