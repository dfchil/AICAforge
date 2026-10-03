#include "afx_sample_c.h"

#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

int ya2beam_encode(const int16_t *pcm, int n, int width, uint8_t *out);

static int16_t read_pcm16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static unsigned rate_gcd(unsigned a, unsigned b) {
    while (b) { unsigned next = a % b; a = b; b = next; }
    return a;
}

/* Modified Bessel I0.  The short positive series is stable for the fixed
 * Kaiser beta=5 used by scipy.signal.resample_poly. */
static double i0(double value) {
    double term = 1.0, sum = 1.0;
    double square = value * value / 4.0;
    for (unsigned k = 1; k < 64; ++k) {
        term *= square / ((double)k * k);
        sum += term;
        if (term <= sum * 1e-16) break;
    }
    return sum;
}

static int16_t round_pcm16(double value) {
    value = nearbyint(value);
    if (value < -32768.0) return -32768;
    if (value > 32767.0) return 32767;
    return (int16_t)value;
}

int afx_c_resample_pcm16(const uint8_t *pcm16, uint32_t frames,
                         uint32_t source_rate, uint32_t target_rate,
                         uint8_t **out_pcm16, uint32_t *out_frames) {
    unsigned gcd, up, down, max_rate, half, taps, pre_pad, remove;
    uint32_t result_frames;
    double *filter = NULL;
    uint8_t *result = NULL;
    if (!pcm16 || !frames || !source_rate || !target_rate || !out_pcm16 || !out_frames) return -1;
    if (source_rate == target_rate) {
        result = malloc((size_t)frames * 2u);
        if (!result) return -1;
        memcpy(result, pcm16, (size_t)frames * 2u);
        *out_pcm16 = result; *out_frames = frames; return 0;
    }
    gcd = rate_gcd(source_rate, target_rate);
    up = target_rate / gcd; down = source_rate / gcd;
    if ((uint64_t)frames * up > UINT32_MAX * (uint64_t)down) return -1;
    result_frames = (uint32_t)(((uint64_t)frames * up + down - 1u) / down);
    max_rate = up > down ? up : down;
    if (max_rate > (UINT_MAX - 1u) / 20u) return -1;
    half = 10u * max_rate;
    taps = 2u * half + 1u;
    pre_pad = down - half % down;
    remove = (half + pre_pad) / down;
    if (taps > UINT32_MAX - pre_pad || result_frames > UINT32_MAX - remove ||
        (uint64_t)(taps + pre_pad) > SIZE_MAX / sizeof(*filter) ||
        (uint64_t)result_frames > SIZE_MAX / 2u) return -1;
    filter = calloc((size_t)taps + pre_pad, sizeof(*filter));
    result = malloc((size_t)result_frames * 2u);
    if (!filter || !result) goto failed;
    double sum = 0.0, denominator = i0(5.0);
    for (unsigned index = 0; index < taps; ++index) {
        double distance = (double)index - half;
        double x = distance / max_rate;
        double sinc = distance == 0.0 ? 1.0 : sin(acos(-1.0) * x) / (acos(-1.0) * x);
        double ratio = distance / half;
        double window = i0(5.0 * sqrt(fmax(0.0, 1.0 - ratio * ratio))) / denominator;
        filter[pre_pad + index] = sinc / max_rate * window;
        sum += filter[pre_pad + index];
    }
    for (unsigned index = pre_pad; index < pre_pad + taps; ++index) filter[index] *= up / sum;
    for (uint32_t output = 0; output < result_frames; ++output) {
        uint64_t sample_at = (uint64_t)(output + remove) * down;
        uint64_t first = sample_at >= taps + pre_pad - 1u ?
                         (sample_at - (taps + pre_pad - 1u) + up - 1u) / up : 0;
        uint64_t last = sample_at / up;
        double value = 0.0;
        if (last >= frames) last = frames - 1u;
        for (uint64_t input = first; input <= last; ++input) {
            uint64_t coefficient = sample_at - input * up;
            if (coefficient < taps + pre_pad)
                value += read_pcm16(pcm16 + 2u * input) * filter[coefficient];
        }
        int16_t rounded = round_pcm16(value);
        result[2u * output] = (uint8_t)rounded;
        result[2u * output + 1u] = (uint8_t)(rounded >> 8);
    }
    free(filter); *out_pcm16 = result; *out_frames = result_frames; return 0;
failed:
    free(filter); free(result); return -1;
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

int afx_c_encode_sample_auto(const uint8_t *pcm16, uint32_t frames,
                             double minimum_snr, double minimum_attack_snr,
                             uint8_t fallback_format,
                             uint8_t **out_data, uint32_t *out_bytes,
                             uint8_t *out_format) {
    uint8_t *data = NULL;
    int16_t *source = NULL, *decoded = NULL;
    if (!pcm16 || !frames || frames > 65535 || fallback_format > AFX_ADPCM ||
        !out_data || !out_bytes || !out_format) return -1;
    source = malloc((size_t)frames * sizeof(*source));
    data = malloc((frames + 1u) / 2u);
    decoded = malloc((size_t)frames * sizeof(*decoded));
    if (!source || !data || !decoded) goto failed;
    for (uint32_t i = 0; i < frames; ++i) source[i] = read_pcm16(pcm16 + 2u * i);
    if (ya2beam_encode(source, (int)frames, 32, data)) goto failed;
    adpcm_decode(data, frames, decoded);
    if (meets_quality(pcm16, decoded, frames, minimum_snr, minimum_attack_snr)) {
        free(source); free(decoded);
        *out_data = data; *out_bytes = (frames + 1u) / 2u; *out_format = AFX_ADPCM; return 0;
    }
    free(source); free(decoded); free(data);
    return afx_c_encode_sample(pcm16, frames, 0, fallback_format, out_data, out_bytes, out_format);
failed:
    free(source); free(data); free(decoded); return -1;
}

int afx_c_encode_sample(const uint8_t *pcm16, uint32_t frames, int looping, uint8_t requested_format,
                        uint8_t **out_data, uint32_t *out_bytes, uint8_t *out_format) {
    uint8_t *data = NULL;
    int16_t *source = NULL;
    if (!pcm16 || !frames || frames > 65535 || requested_format > AFX_SAMPLE_AUTO ||
        !out_data || !out_bytes || !out_format) return -1;
    (void)looping;
    if (requested_format == AFX_SAMPLE_AUTO)
        return afx_c_encode_sample_auto(pcm16, frames, 30.0, 24.0, AFX_PCM8, out_data, out_bytes, out_format);
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
    if (!source || !data) { free(source); free(data); return -1; }
    for (uint32_t i = 0; i < frames; ++i) source[i] = read_pcm16(pcm16 + 2u * i);
    if (ya2beam_encode(source, (int)frames, 32, data)) { free(source); free(data); return -1; }
    free(source);
    *out_data = data; *out_bytes = (frames + 1u) / 2u; *out_format = AFX_ADPCM; return 0;
}

int afx_c_encode_sample_at_rate(const uint8_t *pcm16, uint32_t frames,
                                uint32_t source_rate, uint32_t target_rate, int looping,
                                double minimum_snr, double minimum_attack_snr,
                                uint8_t **out_data, uint32_t *out_bytes,
                                uint8_t *out_format, uint32_t *out_frames) {
    uint8_t *sampled = NULL, *encoded = NULL, *restored = NULL, *best = NULL;
    int16_t *decoded = NULL;
    uint32_t sampled_frames, restored_frames, best_bytes = UINT32_MAX;
    uint8_t best_format = AFX_PCM16;
    if (!out_data || !out_bytes || !out_format || !out_frames ||
        isnan(minimum_snr) || isnan(minimum_attack_snr) ||
        afx_c_resample_pcm16(pcm16, frames, source_rate, target_rate, &sampled, &sampled_frames)) return -1;
    if (!sampled_frames || sampled_frames > 65535u) { free(sampled); return 1; }
    decoded = malloc((size_t)sampled_frames * sizeof(*decoded));
    if (!decoded) goto failed;
    for (uint8_t format = AFX_PCM16; format <= (looping ? AFX_PCM8 : AFX_ADPCM); ++format) {
        uint32_t bytes; uint8_t actual_format;
        if (afx_c_encode_sample(sampled, sampled_frames, looping, format, &encoded, &bytes, &actual_format)) goto failed;
        if (format == AFX_ADPCM) adpcm_decode(encoded, sampled_frames, decoded);
        else for (uint32_t i = 0; i < sampled_frames; ++i)
            decoded[i] = format == AFX_PCM16 ? read_pcm16(encoded + 2u * i) : (int8_t)encoded[i] * 256;
        if (afx_c_resample_pcm16((const uint8_t *)decoded, sampled_frames, target_rate, source_rate,
                                 &restored, &restored_frames)) goto failed;
        uint32_t compared = frames < restored_frames ? frames : restored_frames;
        if (bytes < best_bytes && meets_quality(pcm16, (const int16_t *)restored, compared,
                                                minimum_snr, minimum_attack_snr)) {
            free(best); best = encoded; encoded = NULL; best_bytes = bytes; best_format = actual_format;
        }
        free(encoded); encoded = NULL; free(restored); restored = NULL;
    }
    free(sampled); free(decoded);
    if (!best) return 1;
    *out_data = best; *out_bytes = best_bytes; *out_format = best_format; *out_frames = sampled_frames; return 0;
failed:
    free(sampled); free(encoded); free(restored); free(best); free(decoded); return -1;
}
