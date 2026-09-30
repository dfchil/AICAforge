#include "afx_compile_c.h"
#include "afx_midi_c.h"

#include <stdio.h>
#include <stdlib.h>

static uint8_t *read_file(const char *path, uint32_t *out_bytes) {
    FILE *in = fopen(path, "rb");
    long size;
    uint8_t *data;
    if (!in || fseek(in, 0, SEEK_END) || (size = ftell(in)) < 0 ||
        (uint64_t)size > UINT32_MAX || fseek(in, 0, SEEK_SET)) goto failed;
    data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, in) != (size_t)size) { free(data); goto failed; }
    fclose(in); *out_bytes = (uint32_t)size; return data;
failed:
    if (in) fclose(in);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 4 && argc != 5)
        return fprintf(stderr, "usage: %s source.mid [sample.pcm] out.afb out.afx\n", argv[0]), 2;
    uint32_t midi_bytes;
    uint8_t *data = read_file(argv[1], &midi_bytes);
    if (!data) return fprintf(stderr, "cannot read %s\n", argv[1]), 2;
    afx_c_note_t *notes = NULL; uint32_t count = 0;
    int parsed = afx_c_midi_notes(data, midi_bytes, 1000, &notes, &count);
    free(data);
    afx_c_output_t out;
    uint8_t *pcm = NULL; uint32_t pcm_bytes = 0;
    if (!parsed && argc == 5) pcm = read_file(argv[2], &pcm_bytes);
    afx_c_pcm16_t sample = {pcm, pcm_bytes / 2, 69, 0, 0, pcm_bytes / 2 ? pcm_bytes / 2 - 1 : 0};
    int result = parsed || (argc == 5 && (!pcm || (pcm_bytes & 1))) ? -1 :
                 argc == 5 ? afx_c_compile_pcm16(notes, count, 1000, &sample, &out) :
                 afx_c_compile_sine(notes, count, 1000, &out);
    free(notes);
    free(pcm);
    if (result) return fprintf(stderr, "cannot compile source\n"), 1;
    FILE *afb = fopen(argv[argc - 2], "wb"), *afx = fopen(argv[argc - 1], "wb");
    result = !afb || !afx || fwrite(out.afb, 1, out.afb_bytes, afb) != out.afb_bytes || fwrite(out.afx, 1, out.afx_bytes, afx) != out.afx_bytes;
    if (afb) fclose(afb);
    if (afx) fclose(afx);
    afx_c_output_free(&out);
    return result ? 1 : 0;
}
