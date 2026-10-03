#include "afx_compile_c.h"

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

static int wilhelm_pair(const char *pcm_path, const char *afb, const char *afx) {
    uint8_t *pcm; uint32_t bytes;
    if (read_file(pcm_path, &pcm, &bytes)) return -1;
    const afx_c_note_t note = {.start_tick = 1000, .end_tick = 3000, .key = 69, .velocity = 127};
    const afx_c_sample_t sample = {pcm, bytes, bytes / 2u, AFX_PCM16, 69, 0,
                                   0, (uint16_t)(bytes / 2u - 1u), 1200, 44100};
    afx_c_output_t out;
    int result = afx_c_compile_sample(&note, 1, 1000, &sample, &out);
    free(pcm);
    return result || write_output(afb, afx, &out);
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "quickstart")) {
        const afx_c_note_t notes[] = {{.start_tick = 0, .end_tick = 4320, .key = 69, .velocity = 100},
                                      {.start_tick = 4800, .end_tick = 5280, .key = 81, .velocity = 100},
                                      {.start_tick = 7680, .end_tick = 8160, .key = 84, .velocity = 100}};
        return sine_pair(notes, 3, argv[2], argv[3]) ? 1 : 0;
    }
    if (argc == 4 && !strcmp(argv[1], "dsp-demo")) {
        const afx_c_note_t notes[] = {{.start_tick = 500, .end_tick = 688, .key = 72, .velocity = 110},
                                      {.start_tick = 1000, .end_tick = 1188, .key = 76, .velocity = 110},
                                      {.start_tick = 1500, .end_tick = 1688, .key = 79, .velocity = 110},
                                      {.start_tick = 2000, .end_tick = 2188, .key = 84, .velocity = 110}};
        return sine_pair(notes, 4, argv[2], argv[3]) ? 1 : 0;
    }
    if (argc == 4 && !strcmp(argv[1], "dsp-effects")) {
        const afx_c_note_t phrase[] = {{.start_tick = 0, .end_tick = 375, .key = 72, .velocity = 110},
                                       {.start_tick = 375, .end_tick = 750, .key = 76, .velocity = 110},
                                       {.start_tick = 750, .end_tick = 1125, .key = 79, .velocity = 110},
                                       {.start_tick = 1125, .end_tick = 1500, .key = 84, .velocity = 110}};
        const afx_c_note_t impulse[] = {{.start_tick = 0, .end_tick = 375, .key = 96, .velocity = 110}};
        const afx_c_note_t tone[] = {{.start_tick = 0, .end_tick = 750, .key = 69, .velocity = 110}};
        char afb[4096], afx[4096];
#define PAIR(name, notes) do { \
        if (snprintf(afb, sizeof(afb), "%s/" name ".afb", argv[2]) >= (int)sizeof(afb) || \
            snprintf(afx, sizeof(afx), "%s/" name ".afx", argv[2]) >= (int)sizeof(afx) || \
            sine_pair(notes, (uint32_t)(sizeof(notes) / sizeof(*(notes))), afb, afx)) return 1; \
    } while (0)
        PAIR("effect", phrase); PAIR("impulse", impulse); PAIR("tone", tone); PAIR("modulated", phrase);
#undef PAIR
        if (snprintf(afb, sizeof(afb), "%s/wilhelm.afb", argv[2]) >= (int)sizeof(afb) ||
            snprintf(afx, sizeof(afx), "%s/wilhelm.afx", argv[2]) >= (int)sizeof(afx) ||
            wilhelm_pair(argv[3], afb, afx)) return 1;
        return 0;
    }
    return fprintf(stderr, "usage: %s quickstart|dsp-demo out.afb out.afx\n"
                   "       %s dsp-effects output-dir wilhelm.pcm\n", argv[0], argv[0]), 2;
}
