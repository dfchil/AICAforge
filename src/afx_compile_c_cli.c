#include "afx_compile_c.h"
#include "afx_midi_c.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 4) return fprintf(stderr, "usage: %s source.mid out.afb out.afx\n", argv[0]), 2;
    FILE *in = fopen(argv[1], "rb");
    long size;
    if (!in || fseek(in, 0, SEEK_END) || (size = ftell(in)) < 0 ||
        (uint64_t)size > UINT32_MAX || fseek(in, 0, SEEK_SET)) {
        if (in) fclose(in);
        return fprintf(stderr, "cannot read %s\n", argv[1]), 2;
    }
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, in) != (size_t)size) {
        free(data); fclose(in); return fprintf(stderr, "cannot read %s\n", argv[1]), 2;
    }
    fclose(in);
    afx_c_note_t *notes = NULL; uint32_t count = 0;
    int parsed = afx_c_midi_notes(data, (uint32_t)size, 1000, &notes, &count);
    free(data);
    afx_c_output_t out;
    int result = parsed ? -1 : afx_c_compile_sine(notes, count, 1000, &out);
    free(notes);
    if (result) return fprintf(stderr, "invalid timeline\n"), 1;
    FILE *afb = fopen(argv[2], "wb"), *afx = fopen(argv[3], "wb");
    result = !afb || !afx || fwrite(out.afb, 1, out.afb_bytes, afb) != out.afb_bytes || fwrite(out.afx, 1, out.afx_bytes, afx) != out.afx_bytes;
    if (afb) fclose(afb);
    if (afx) fclose(afx);
    afx_c_output_free(&out);
    return result ? 1 : 0;
}
