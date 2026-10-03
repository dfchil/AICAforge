#ifndef AICAFLOW_SF2_C_H
#define AICAFLOW_SF2_C_H

#include "afx_compile_c.h"

#define AFX_C_SAMPLE_NAME_BYTES 21u

/* Resolve every MIDI note through the matching SF2 preset/instrument zones.
 * Notes are expanded when a SoundFont deliberately layers regions; the
 * returned notes then carry explicit setup indices, so no runtime SF2 lookup
 * exists. The caller supplies an explicit pcm16/pcm8/adpcm/auto policy and
 * may cap decoded sample rate; zero preserves the SoundFont source rate.
 * filter_offset_cents is an offline per-map adjustment applied before AICA
 * filter registers are generated. gain_bias_centibels is an explicit offline
 * calibration for a mapping; zero preserves the SoundFont's own level.
 */
typedef struct {
    afx_c_note_t *notes;
    afx_c_zone_t *zones;
    char (*zone_names)[AFX_C_SAMPLE_NAME_BYTES];
    uint8_t **owned_samples;
    uint32_t note_count, zone_count, owned_count;
} afx_c_sf2_output_t;

enum { AFX_C_SF2_STEREO, AFX_C_SF2_LEFT, AFX_C_SF2_RIGHT };
enum { AFX_C_SF2_FILTER_ENVELOPE, AFX_C_SF2_FILTER_STATIC, AFX_C_SF2_FILTER_NONE };
enum { AFX_C_SF2_GAIN_STANDARD, AFX_C_SF2_GAIN_FLUIDSYNTH2 };
enum { AFX_C_SF2_ENVELOPE_SF2, AFX_C_SF2_ENVELOPE_FIXED };

/* AFBM owns these source-to-AICA choices.  Keeping them together avoids an
 * ever-growing positional resolver API and keeps AFP independent of samples. */
typedef struct {
    uint8_t sample_format, channel, filter_model, gain_model, envelope_model;
    uint8_t source_pan, source_reverb, source_modulators, has_dsp_send, dsp_send;
    uint8_t has_direct, has_lfo;
    uint16_t direct, lfo;
    uint32_t sample_rate_cap, loop_ms;
    int filter_offset_cents, gain_bias_centibels;
} afx_c_sf2_options_t;

void afx_c_sf2_options_default(afx_c_sf2_options_t *out);

int afx_c_sf2_resolve(const char *path, const afx_c_note_t *notes, uint32_t count,
                      const afx_c_sf2_options_t *options,
                      afx_c_sf2_output_t *out);
void afx_c_sf2_output_free(afx_c_sf2_output_t *out);

#endif
