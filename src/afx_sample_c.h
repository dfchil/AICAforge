#ifndef AICAFLOW_SAMPLE_C_H
#define AICAFLOW_SAMPLE_C_H

#include "afx_compile_c.h"

/* Map-file names deliberately describe an authoring policy, never a runtime
 * format variant. `auto` chooses the smallest coding passing the documented
 * offline quality gate. */
enum { AFX_SAMPLE_AUTO = 3 };

int afx_c_parse_sample_format(const char *name, uint8_t *out_format);
const char *afx_c_sample_format_name(uint8_t format);

/* Offline Kaiser-windowed polyphase PCM16 resampling.  It matches the
 * retired Python reference's resample_poly policy without making SciPy a
 * release-tool dependency.  The caller owns *out_pcm16. */
int afx_c_resample_pcm16(const uint8_t *pcm16, uint32_t frames,
                         uint32_t source_rate, uint32_t target_rate,
                         uint8_t **out_pcm16, uint32_t *out_frames);

/* Convert little-endian signed PCM16 to the requested AICA representation.
 * The caller owns *out_data. `auto` tries ADPCM, then the PCM8 baseline. */
int afx_c_encode_sample(const uint8_t *pcm16, uint32_t frames, int looping, uint8_t requested_format,
                        uint8_t **out_data, uint32_t *out_bytes, uint8_t *out_format);

/* Shared offline `auto` policy hook.  Importers choose their documented
 * quality gate and fallback coding, while all use the same encoder and SNR
 * measurement.  Looping sources normally select their explicit format. */
int afx_c_encode_sample_auto(const uint8_t *pcm16, uint32_t frames,
                             double minimum_snr, double minimum_attack_snr,
                             uint8_t fallback_format,
                             uint8_t **out_data, uint32_t *out_bytes,
                             uint8_t *out_format);

#endif
