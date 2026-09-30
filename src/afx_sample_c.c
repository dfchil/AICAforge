#include "afx_sample_c.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int ya2beam_encode(const int16_t *pcm, int n, int width, uint8_t *out);

static int16_t read_pcm16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static void adpcm_decode(const uint8_t *data, uint32_t frames, int16_t *out) {
    static const int diff[16] = {1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15};
    static const int scale[8] = {230, 230, 230, 230, 307, 409, 512, 614};
    int current = 0, quant = 127;
    for (uint32_t i = 0; i < frames; ++i) {
        int code = (data[i >> 1] >> ((i & 1u) * 4u)) & 15;
        current += quant * diff[code] / 8;
        if (current < -32768) current = -32768; else if (current > 32767) current = 32767;
        quant = quant * scale[code & 7] >> 8;
        if (quant < 127) quant = 127; else if (quant > 24576) quant = 24576;
        out[i] = (int16_t)current;
    }
}

static int meets_quality(const uint8_t *source, const int16_t *restored, uint32_t frames,
                         double minimum_snr, double minimum_attack_snr) {
    double signal = 0, noise = 0, attack_signal = 0, attack_noise = 0;
    uint32_t attack = frames < 1024 ? frames : 1024;
    for (uint32_t i = 0; i < frames; ++i) {
        double original = read_pcm16(source + 2u * i), error = original - restored[i];
        signal += original * original; noise += error * error;
        if (i < attack) { attack_signal += original * original; attack_noise += error * error; }
    }
    double snr = noise ? 10.0 * log10((signal > 1 ? signal : 1) / noise) : INFINITY;
    double attack_snr = attack_noise ? 10.0 * log10((attack_signal > 1 ? attack_signal : 1) / attack_noise) : INFINITY;
    return snr >= minimum_snr && attack_snr >= minimum_attack_snr;
}

int afx_c_parse_sample_format(const char *name, uint8_t *out_format) {
    if (!strcmp(name, "pcm16")) *out_format = AFX_PCM16;
    else if (!strcmp(name, "pcm8")) *out_format = AFX_PCM8;
    else if (!strcmp(name, "adpcm")) *out_format = AFX_ADPCM;
    else if (!strcmp(name, "auto")) *out_format = AFX_SAMPLE_AUTO;
    else return -1;
    return 0;
}

const char *afx_c_sample_format_name(uint8_t format) {
    static const char *const names[] = {"pcm16", "pcm8", "adpcm", "auto"};
    return format <= AFX_SAMPLE_AUTO ? names[format] : "invalid";
}

int afx_c_encode_sample(const uint8_t *pcm16, uint32_t frames, int looping, uint8_t requested_format,
                        uint8_t **out_data, uint32_t *out_bytes, uint8_t *out_format) {
    uint8_t *data = NULL;
    int16_t *source = NULL, *decoded = NULL;
    if (!pcm16 || !frames || frames > 65535 || requested_format > AFX_SAMPLE_AUTO ||
        !out_data || !out_bytes || !out_format) return -1;
    if (requested_format == AFX_PCM16) {
        data = malloc((size_t)frames * 2u);
        if (!data) return -1;
        memcpy(data, pcm16, (size_t)frames * 2u);
        *out_data = data; *out_bytes = frames * 2u; *out_format = AFX_PCM16; return 0;
    }
    if (requested_format == AFX_PCM8) {
        data = malloc(frames);
        if (!data) return -1;
        for (uint32_t i = 0; i < frames; ++i) data[i] = pcm16[2u * i + 1u];
        *out_data = data; *out_bytes = frames; *out_format = AFX_PCM8; return 0;
    }
    source = malloc((size_t)frames * sizeof(*source));
    data = malloc((frames + 1u) / 2u);
    decoded = malloc((size_t)frames * sizeof(*decoded));
    if (!source || !data || !decoded) goto failed;
    for (uint32_t i = 0; i < frames; ++i) source[i] = read_pcm16(pcm16 + 2u * i);
    if (ya2beam_encode(source, (int)frames, 32, data)) goto failed;
    adpcm_decode(data, frames, decoded);
    if (requested_format == AFX_ADPCM || (!looping && meets_quality(pcm16, decoded, frames, 30.0, 24.0))) {
        free(source); free(decoded);
        *out_data = data; *out_bytes = (frames + 1u) / 2u; *out_format = AFX_ADPCM; return 0;
    }
    free(source); free(decoded); free(data);
    /* PCM8 is the established AICAflow baseline; its 8-bit conversion is
     * audibly robust for the existing DKR and music banks. PCM16 stays an
     * explicit map choice, rather than allowing `auto` to exceed AICA RAM. */
    return afx_c_encode_sample(pcm16, frames, looping, AFX_PCM8, out_data, out_bytes, out_format);
failed:
    free(source); free(data); free(decoded); return -1;
}
