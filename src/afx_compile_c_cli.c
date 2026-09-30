#include "afx_compile_c.h"
#include "afx_midi_c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void free_zones(afx_c_zone_t *zones, uint8_t **owned, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) free(owned[i]);
    free(owned); free(zones);
}

static int load_zones(const char *path, afx_c_zone_t **out_zones, uint8_t ***out_owned,
                      uint32_t *out_count) {
    FILE *file = fopen(path, "r");
    afx_c_zone_t *zones = NULL; uint8_t **owned = NULL;
    uint32_t count = 0, capacity = 0; char line[1200];
    if (!file) return -1;
    while (fgets(line, sizeof(line), file)) {
        unsigned key_min, key_max, root; int loop_start, loop_end; char sample_path[1024];
        char *first = line;
        while (*first == ' ' || *first == '\t') ++first;
        int fields = sscanf(line, " %u %u %u %d %d %1023s", &key_min, &key_max, &root,
                            &loop_start, &loop_end, sample_path);
        if (!fields && (*first == '#' || *first == '\n' || !*first)) continue;
        if (fields != 6 || key_min > key_max || key_max > 127 || root > 127) goto failed;
        if (count == capacity) {
            uint32_t next = capacity ? capacity * 2u : 8u;
            afx_c_zone_t *new_zones = realloc(zones, (size_t)next * sizeof(*zones));
            if (!new_zones) goto failed;
            zones = new_zones;
            uint8_t **new_owned = realloc(owned, (size_t)next * sizeof(*owned));
            if (!new_owned) goto failed;
            owned = new_owned; capacity = next;
        }
        uint32_t bytes = 0; uint8_t *pcm = read_file(sample_path, &bytes);
        if (!pcm || !bytes || (bytes & 1) || bytes / 2 > 65535) { free(pcm); goto failed; }
        afx_c_pcm16_t sample = {pcm, bytes / 2, (uint8_t)root, 0, 0, (uint16_t)(bytes / 2 - 1)};
        if (loop_start == -1 && loop_end == -1) { }
        else if (loop_start >= 0 && loop_end >= loop_start && (uint32_t)loop_end < sample.frames) {
            sample.loop = 1; sample.loop_start = (uint16_t)loop_start; sample.loop_end = (uint16_t)loop_end;
        } else { free(pcm); goto failed; }
        zones[count] = (afx_c_zone_t){sample, (uint8_t)key_min, (uint8_t)key_max};
        owned[count++] = pcm;
    }
    fclose(file);
    if (!count) { free_zones(zones, owned, count); return -1; }
    *out_zones = zones; *out_owned = owned; *out_count = count; return 0;
failed:
    fclose(file); free_zones(zones, owned, count); return -1;
}

int main(int argc, char **argv) {
    int zones_mode = argc == 6 && !strcmp(argv[2], "--zones");
    if ((argc != 4 && argc != 5) && !zones_mode)
        return fprintf(stderr, "usage: %s source.mid [sample.pcm] out.afb out.afx\n"
                       "       %s source.mid --zones zones.txt out.afb out.afx\n", argv[0], argv[0]), 2;
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
    afx_c_zone_t *zones = NULL; uint8_t **owned = NULL; uint32_t zone_count = 0;
    if (!parsed && zones_mode && load_zones(argv[3], &zones, &owned, &zone_count)) parsed = -1;
    int result = parsed || (argc == 5 && (!pcm || (pcm_bytes & 1))) ? -1 :
                 zones_mode ? afx_c_compile_zones(notes, count, 1000, zones, zone_count, &out) :
                 argc == 5 ? afx_c_compile_pcm16(notes, count, 1000, &sample, &out) :
                 afx_c_compile_sine(notes, count, 1000, &out);
    free(notes);
    free(pcm);
    free_zones(zones, owned, zone_count);
    if (result) return fprintf(stderr, "cannot compile source\n"), 1;
    FILE *afb = fopen(argv[argc - 2], "wb"), *afx = fopen(argv[argc - 1], "wb");
    result = !afb || !afx || fwrite(out.afb, 1, out.afb_bytes, afb) != out.afb_bytes || fwrite(out.afx, 1, out.afx_bytes, afx) != out.afx_bytes;
    if (afb) fclose(afb);
    if (afx) fclose(afx);
    afx_c_output_free(&out);
    return result ? 1 : 0;
}
