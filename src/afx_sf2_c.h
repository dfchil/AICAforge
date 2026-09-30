#ifndef AICAFLOW_SF2_C_H
#define AICAFLOW_SF2_C_H

#include "afx_compile_c.h"

/* Resolve every MIDI note through the matching SF2 preset/instrument zones.
 * Notes are expanded when a SoundFont deliberately layers regions; the
 * returned notes then carry explicit setup indices, so no runtime SF2 lookup
 * exists. The caller supplies an explicit pcm16/pcm8/adpcm/auto policy.
 */
typedef struct {
    afx_c_note_t *notes;
    afx_c_zone_t *zones;
    uint8_t **owned_samples;
    uint32_t note_count, zone_count, owned_count;
} afx_c_sf2_output_t;

int afx_c_sf2_resolve(const char *path, const afx_c_note_t *notes, uint32_t count,
                      uint8_t sample_format, afx_c_sf2_output_t *out);
void afx_c_sf2_output_free(afx_c_sf2_output_t *out);

#endif
