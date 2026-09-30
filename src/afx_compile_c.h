#ifndef AICAFLOW_COMPILE_C_H
#define AICAFLOW_COMPILE_C_H

#include <stdint.h>

#include <aicaflow/protocol.h>

typedef struct {
    uint32_t start_tick, end_tick;
    uint8_t key, velocity;
    /* MIDI bank/program are resolved at NOTE-on. They are authoring data only:
     * the emitted AFX contains setup indices, never MIDI lookups. */
    uint8_t bank_msb, bank_lsb, program;
    /* Zero selects a zone by MIDI attributes. Nonzero is a one-based resolved
     * zone index used by importers which expand layered SoundFont notes. */
    uint16_t setup_index;
} afx_c_note_t;

typedef struct {
    uint8_t *afb, *afx, *afc, *afv;
    uint32_t afb_bytes, afx_bytes, afc_bytes, afv_bytes;
} afx_c_output_t;

/* One explicit AICA sample. `loop_end` is inclusive, as in AICA's LEA
 * register. `frames` is decoded PCM frame count even for ADPCM; `bytes` is
 * the contiguous AFB payload size. A non-looping source still has valid loop
 * bounds but clears looping. */
typedef struct {
    const uint8_t *data;
    uint32_t bytes, frames;
    uint8_t format, root_key, loop;
    uint16_t loop_start, loop_end;
    int16_t tuning_cents;
    /* Zero means the normal 44.1 kHz AICA base rate. */
    uint32_t sample_rate;
} afx_c_sample_t;

typedef struct {
    afx_c_sample_t sample;
    uint8_t key_min, key_max, velocity_min, velocity_max;
    uint8_t bank_msb, bank_lsb, program;
    /* AICA DSP-send register byte: IMXL in the high nibble, ISEL in low. */
    uint8_t dsp_send;
} afx_c_zone_t;

/* Every note must select exactly one key range. The zones become the AFB's
 * contiguous, 32-byte aligned samples and the AFX setup dictionary. */
int afx_c_compile_zones(const afx_c_note_t *notes, uint32_t count,
                        uint32_t tick_rate, const afx_c_zone_t *zones,
                        uint32_t zone_count, afx_c_output_t *out);

/* Compile a resolved timeline against one explicit sample. All output sidecars
 * are allocated with the AFB/AFX pair and released by afx_c_output_free(). */
int afx_c_compile_sample(const afx_c_note_t *notes, uint32_t count,
                         uint32_t tick_rate, const afx_c_sample_t *sample,
                         afx_c_output_t *out);

/* Compile a resolved monophonic/polyphonic note timeline using the built-in
 * sine source. The caller owns out through afx_c_output_free(). */
int afx_c_compile_sine(const afx_c_note_t *notes, uint32_t count,
                       uint32_t tick_rate, afx_c_output_t *out);
void afx_c_output_free(afx_c_output_t *out);

#endif
