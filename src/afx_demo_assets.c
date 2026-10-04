#include "afx_compile_c.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_file(const char *path, const uint8_t *data, uint32_t bytes) {
    FILE *file = fopen(path, "wb");
    int failed = !file || fwrite(data, 1, bytes, file) != bytes;
    if (file && fclose(file)) failed = 1;
    return failed ? -1 : 0;
}

static int sidecar(const char *afx, const char *extension, char out[4096]) {
    size_t bytes = strlen(afx);
    if (bytes < 4 || strcmp(afx + bytes - 4, ".afx") || bytes - 4 + strlen(extension) >= 4096) return -1;
    memcpy(out, afx, bytes - 4); strcpy(out + bytes - 4, extension); return 0;
}

static int write_output(const char *afb, const char *afx, afx_c_output_t *out) {
    char afc[4096], afv[4096];
    int failed = sidecar(afx, ".afc", afc) || sidecar(afx, ".afv", afv) ||
                 write_file(afb, out->afb, out->afb_bytes) || write_file(afx, out->afx, out->afx_bytes) ||
                 write_file(afc, out->afc, out->afc_bytes) || write_file(afv, out->afv, out->afv_bytes);
    afx_c_output_free(out); return failed ? -1 : 0;
}

static int sine_pair(const afx_c_note_t *notes, uint32_t count, const char *afb, const char *afx) {
    afx_c_output_t out;
    return afx_c_compile_sine(notes, count, 1000, &out) || write_output(afb, afx, &out);
}

static int multiple_dsp_effects(const char *afb, const char *afx) {
    int16_t click[128], tone[128];
    for (uint32_t i = 0; i < 128; ++i) {
        click[i] = (int16_t)(sin(6.28318530717958647692 * 6 * i / 128) * (127 - i) * 220);
        tone[i] = (int16_t)(sin(6.28318530717958647692 * i / 128) * 28000);
    }
    const afx_c_note_t notes[] = {
        {.start_tick = 500, .end_tick = 520, .key = 69, .velocity = 127, .setup_index = 1},
        {.start_tick = 1000, .end_tick = 1020, .key = 69, .velocity = 127, .setup_index = 1},
        {.start_tick = 1500, .end_tick = 1520, .key = 69, .velocity = 127, .setup_index = 1},
        {.start_tick = 2000, .end_tick = 2020, .key = 69, .velocity = 127, .setup_index = 1},
        {.start_tick = 3000, .end_tick = 5000, .key = 69, .velocity = 127, .setup_index = 2},
        {.start_tick = 6000, .end_tick = 6020, .key = 69, .velocity = 127, .setup_index = 1},
        {.start_tick = 6500, .end_tick = 6520, .key = 69, .velocity = 127, .setup_index = 1},
        {.start_tick = 7000, .end_tick = 7020, .key = 69, .velocity = 127, .setup_index = 1},
        {.start_tick = 6000, .end_tick = 8000, .key = 69, .velocity = 127, .setup_index = 2},
    };
    const afx_c_zone_t zones[] = {
        {.sample = {(const uint8_t *)click, sizeof(click), 128, AFX_PCM16, 69, 0, 0, 127, 0, 44100},
         .key_max = 127, .velocity_max = 127, .dsp_send = 0xf0,
         .setup_mask = 1u << AFX_FIELD_DIRECT, .setup = {[AFX_FIELD_DIRECT] = 0}},
        {.sample = {(const uint8_t *)tone, sizeof(tone), 128, AFX_PCM16, 69, 1, 0, 127, 0, 22050},
         .key_max = 127, .velocity_max = 127, .dsp_send = 0xf1,
         .setup_mask = 1u << AFX_FIELD_DIRECT, .setup = {[AFX_FIELD_DIRECT] = 0}},
    };
    afx_c_output_t out;
    return afx_c_compile_zones(notes, sizeof(notes) / sizeof(*notes), 1000, zones,
                               sizeof(zones) / sizeof(*zones), &out) || write_output(afb, afx, &out);
}

static int read_file(const char *path, uint8_t **out, uint32_t *out_bytes) {
    FILE *file = fopen(path, "rb"); long bytes; uint8_t *data;
    if (!file || fseek(file, 0, SEEK_END) || (bytes = ftell(file)) < 2 || (bytes & 1) ||
        bytes > 131070 || fseek(file, 0, SEEK_SET)) goto failed;
    data = malloc((size_t)bytes);
    if (!data || fread(data, 1, (size_t)bytes, file) != (size_t)bytes) { free(data); goto failed; }
    fclose(file); *out = data; *out_bytes = (uint32_t)bytes; return 0;
failed:
    if (file) fclose(file); return -1;
}

static int dsp_inputs(const char *directory, const char *pcm_path) {
    enum { AUDIO, CONTROL1, CONTROL2, CONTROL3, CONTROL4, WILHELM, SLOW_LFO, ZONES };
    uint8_t *pcm; uint32_t bytes;
    if (read_file(pcm_path, &pcm, &bytes)) return -1;
    int16_t sine[104];
    int16_t slow_lfo[2048];
    afx_c_zone_t zones[ZONES] = {0};
    /* Original DSP listener waveform, including its four loop guard frames. */
    for (unsigned i = 0; i < 104; ++i)
        sine[i] = (int16_t)(6000 * sin(6.28318530717958647692 * (i % 100) / 100));
    for (unsigned i = 0; i < 2048; ++i)
        slow_lfo[i] = (int16_t)(30000 * sin(6.28318530717958647692 * i / 2048));
    for (unsigned i = 0; i < ZONES; ++i) {
        unsigned frames = i == WILHELM ? bytes / 2 : 104;
        zones[i] = (afx_c_zone_t){
            .sample = {i == WILHELM ? pcm : (const uint8_t *)sine, frames * 2, frames,
                       AFX_PCM16, 69, i != WILHELM, 0,
                       i == WILHELM ? (uint16_t)(frames - 1) : 100, 0,
                       i == WILHELM ? 22050 : 44100},
            .key_max = 127, .velocity_max = 127, .dsp_send = 0xf0,
        };
        if (i >= CONTROL1 && i <= CONTROL4) {
            zones[i].dsp_send |= i;
            zones[i].setup_mask = 1u << AFX_FIELD_DIRECT;
            zones[i].setup[AFX_FIELD_DIRECT] = 0x0010; /* No direct control audio. */
        }
    }
    /* Half-Hz control for moving delays/gains; keep the old audio-rate
     * carrier for ring modulation. This voice has no direct output. */
    zones[SLOW_LFO] = (afx_c_zone_t){
        .sample = {(const uint8_t *)slow_lfo, sizeof(slow_lfo), 2048, AFX_PCM16,
                   69, 1, 0, 2047, 0, 1024},
        .key_max = 127, .velocity_max = 127, .dsp_send = 0xf1,
        .setup_mask = 1u << AFX_FIELD_DIRECT, .setup = {[AFX_FIELD_DIRECT] = 0},
    };
    const struct { const char *name; unsigned key, count, controls; } inputs[] = {
        {"effect", 72, 4, 4}, {"impulse", 96, 1, 0}, {"tone", 69, 1, 0},
        {"modulated", 72, 4, 1}, {"wilhelm", 69, 1, 0}, {"slow", 69, 1, 1},
    };
    int result = 0;
    for (unsigned i = 0; !result && i < sizeof(inputs) / sizeof(*inputs); ++i) {
        afx_c_note_t notes[8] = {0};
        unsigned count = inputs[i].count;
        const unsigned phrase[] = {72, 76, 79, 84}, controls[] = {24, 31, 36, 43};
        for (unsigned n = 0; n < count; ++n)
            notes[n] = (afx_c_note_t){.start_tick = i == 4 ? 1000 : (1000 + n * 1750 + 2) / 4,
                .end_tick = i >= 4 ? 4000 : (1750 + (count - 1) * 1750 + 2) / 4 + 2000,
                .release_tick = i >= 4 ? 3000 : (1750 + n * 1750 + 2) / 4,
                .key = count == 4 ? phrase[n] : inputs[i].key,
                /* Leave 6 dB for the wet+dry sum without reducing LFO depth. */
                .mix = i == 5 ? 0x1024 : 0,
                .velocity = 127, .setup_index = i >= 4 ? WILHELM + 1 : AUDIO + 1};
        for (unsigned c = 0; c < inputs[i].controls; ++c)
            notes[count++] = (afx_c_note_t){.start_tick = 250, .end_tick = notes[0].end_tick,
                .release_tick = i == 5 ? 3000 : 1750,
                .key = i == 5 ? 69 : inputs[i].controls == 1 ? 36 : controls[c],
                .velocity = 127, .setup_index = i == 5 ? SLOW_LFO + 1 : CONTROL1 + c + 1};
        afx_c_output_t out;
        /* Every flow sees the same complete zone list, so bank identity and
         * sample offsets are identical. Only its setup dictionary is pruned. */
        result = afx_c_compile_zones(notes, count, 1000, zones, ZONES, &out);
        if (result) break;
        char path[4096];
        if (!i) {
            result = snprintf(path, sizeof(path), "%s/inputs.afb", directory) >= (int)sizeof(path);
            if (!result) result = write_file(path, out.afb, out.afb_bytes);
        }
        if (!result) result = snprintf(path, sizeof(path), "%s/%s.afx", directory, inputs[i].name) >= (int)sizeof(path);
        if (!result) result = write_file(path, out.afx, out.afx_bytes);
        afx_c_output_free(&out);
    }
    free(pcm);
    return result;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "quickstart")) {
        const afx_c_note_t notes[] = {{.start_tick = 0, .end_tick = 4320, .key = 69, .velocity = 100},
                                      {.start_tick = 4800, .end_tick = 5280, .key = 81, .velocity = 100},
                                      {.start_tick = 7680, .end_tick = 8160, .key = 84, .velocity = 100}};
        return sine_pair(notes, 3, argv[2], argv[3]) ? 1 : 0;
    }
    if (argc == 4 && !strcmp(argv[1], "multiple-dsp-effects")) {
        return multiple_dsp_effects(argv[2], argv[3]) ? 1 : 0;
    }
    if (argc == 4 && !strcmp(argv[1], "dsp-effects")) {
        return dsp_inputs(argv[2], argv[3]) ? 1 : 0;
    }
    return fprintf(stderr, "usage: %s quickstart|multiple-dsp-effects out.afb out.afx\n"
                   "       %s dsp-effects output-dir wilhelm.pcm\n", argv[0], argv[0]), 2;
}
