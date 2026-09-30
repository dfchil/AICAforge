#include "afx_compile_c.h"
#include "afx_midi_c.h"
#include "afx_sample_c.h"
#include "afx_sf2_c.h"

#include <aicaflow/codec.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char name[128], path[1024]; uint8_t channel; } source_t;
typedef struct {
    char song[128];
    unsigned midi_bank, midi_program, sf2_bank, sf2_program;
    uint8_t format;
    source_t *source;
} map_t;
typedef struct {
    char name[128], midi[1024];
    afx_c_sf2_output_t resolved;
} song_t;

static int read_file(const char *path, uint8_t **out_data, uint32_t *out_bytes) {
    FILE *file = fopen(path, "rb"); long bytes; uint8_t *data;
    if (!file || fseek(file, 0, SEEK_END) || (bytes = ftell(file)) < 1 ||
        (uint64_t)bytes > UINT32_MAX || fseek(file, 0, SEEK_SET)) goto failed;
    data = malloc((size_t)bytes);
    if (!data || fread(data, 1, (size_t)bytes, file) != (size_t)bytes) { free(data); goto failed; }
    fclose(file); *out_data = data; *out_bytes = (uint32_t)bytes; return 0;
failed:
    if (file) fclose(file); return -1;
}

static int write_file(const char *path, const uint8_t *data, uint32_t bytes) {
    FILE *file = fopen(path, "wb");
    int failed = !file || fwrite(data, 1, bytes, file) != bytes;
    if (file && fclose(file)) failed = 1;
    return failed ? -1 : 0;
}

static int relative_path(const char *map_path, const char *path, char out[1200]) {
    const char *slash = strrchr(map_path, '/');
    size_t dir = slash ? (size_t)(slash - map_path) : 0;
    if (path[0] == '/' || !dir) return snprintf(out, 1200, "%s", path) >= 1200 ? -1 : 0;
    if (dir + 1u + strlen(path) >= 1200) return -1;
    memcpy(out, map_path, dir); out[dir] = '/'; strcpy(out + dir + 1u, path); return 0;
}

static int map_relative_path(const char *map_path, const char *input, char out[1200]) {
    char map_copy[PATH_MAX], map_dir[PATH_MAX], target[PATH_MAX];
    const char *slash = strrchr(map_path, '/');
    if (slash) {
        size_t bytes = (size_t)(slash - map_path);
        if (bytes >= sizeof(map_copy)) return -1;
        memcpy(map_copy, map_path, bytes); map_copy[bytes] = 0;
    } else strcpy(map_copy, ".");
    if (!realpath(map_copy, map_dir) || !realpath(input, target)) return -1;
    size_t common = 0, boundary = 0;
    while (map_dir[common] && map_dir[common] == target[common]) {
        if (map_dir[common] == '/') boundary = common + 1u;
        ++common;
    }
    const char *from = map_dir + boundary, *to = target + boundary;
    size_t used = 0;
    while (*from) {
        const char *next = strchr(from, '/');
        if (used + 3 >= 1200) return -1;
        memcpy(out + used, "../", 3); used += 3;
        from = next ? next + 1 : from + strlen(from);
    }
    if (!*to && !used) { strcpy(out, "."); return 0; }
    if (used + strlen(to) >= 1200) return -1;
    strcpy(out + used, to); return 0;
}

static int flow_paths(const char *directory, const char *name, char afx[4096], char afc[4096], char afv[4096]) {
    return snprintf(afx, 4096, "%s/%s.afx", directory, name) >= 4096 ||
           snprintf(afc, 4096, "%s/%s.afc", directory, name) >= 4096 ||
           snprintf(afv, 4096, "%s/%s.afv", directory, name) >= 4096 ? -1 : 0;
}

static uint32_t align32(uint32_t value) { return (value + 31u) & ~31u; }

static int index_paths(const char *afb, char index[4096], char named[4096]) {
    size_t bytes = strlen(afb);
    if (bytes < 4 || strcmp(afb + bytes - 4, ".afb") || bytes - 4u + 11u >= 4096) return -1;
    return snprintf(index, 4096, "%.*s.afi", (int)(bytes - 4u), afb) >= 4096 ||
           snprintf(named, 4096, "%.*s.names.afi", (int)(bytes - 4u), afb) >= 4096 ? -1 : 0;
}

static int write_afi(const char *path, const uint8_t *afb, uint32_t afb_bytes,
                     const afx_c_zone_t *source, uint32_t source_count,
                     const char (*zone_names)[AFX_C_SAMPLE_NAME_BYTES], int names) {
    uint32_t afb_payload, *offsets = NULL, *zones = NULL;
    if (!path || !afb || !source || !source_count || afb_bytes < AFX_BANK_HEADER_BYTES ||
        afx_read32(afb) != AFX_BANK_MAGIC) return -1;
    afb_payload = afx_read32(afb + 16);
    if (afb_payload != AFX_BANK_HEADER_BYTES) return -1;
    offsets = malloc((size_t)source_count * sizeof(*offsets));
    zones = malloc((size_t)source_count * sizeof(*zones));
    if (!offsets || !zones) goto failed;
    uint32_t count = 0;
    uint32_t bytes = 0;
    for (uint32_t zone = 0; zone < source_count; ++zone) {
        uint32_t sample = 0;
        while (sample < count && (source[zone].sample.data != source[zones[sample]].sample.data ||
                                  source[zone].sample.bytes != source[zones[sample]].sample.bytes)) ++sample;
        if (sample != count) continue;
        bytes = align32(bytes);
        if (!source[zone].sample.bytes || bytes > afx_read32(afb + 20) ||
            source[zone].sample.bytes > afx_read32(afb + 20) - bytes) goto failed;
        offsets[count] = bytes; zones[count++] = zone;
        if (source[zone].sample.bytes > UINT32_MAX - bytes) goto failed;
        bytes += source[zone].sample.bytes;
    }
    uint32_t record_bytes = names ? AFX_INDEX_NAMED_RECORD_BYTES : AFX_INDEX_RECORD_BYTES;
    uint64_t total = (uint64_t)AFX_INDEX_HEADER_BYTES + (uint64_t)count * record_bytes;
    if (total > UINT32_MAX - 31u) goto failed;
    uint32_t file_bytes = align32((uint32_t)total);
    uint8_t *data = calloc(1, file_bytes);
    if (!data) goto failed;
    afx_write32(data, AFX_INDEX_MAGIC); afx_write32(data + 4, AFX_INDEX_VERSION);
    afx_write32(data + 8, afx_read32(afb + 8)); afx_write32(data + 12, afx_read32(afb + 12));
    afx_write32(data + 16, AFX_INDEX_HEADER_BYTES); afx_write32(data + 20, count);
    afx_write32(data + 24, record_bytes); afx_write32(data + 28, file_bytes);
    uint32_t cursor = AFX_INDEX_HEADER_BYTES;
    for (uint32_t sample = 0; sample < count; ++sample) {
        const afx_c_sample_t *entry = &source[zones[sample]].sample;
        afx_write32(data + cursor, afb_payload + offsets[sample]);
        afx_write32(data + cursor + 4, entry->sample_rate ? entry->sample_rate : 44100u);
        afx_write32(data + cursor + 8, entry->frames);
        data[cursor + 12] = entry->format;
        if (names) {
            char fallback[16] = {0};
            const char *name = zone_names && zone_names[zones[sample]][0] ? zone_names[zones[sample]] :
                               (snprintf(fallback, sizeof(fallback), "sample-%03u", sample), fallback);
            memcpy(data + cursor + AFX_INDEX_RECORD_BYTES, name,
                   strlen(name) < 16u ? strlen(name) : 16u);
        }
        cursor += record_bytes;
    }
    int error = write_file(path, data, file_bytes);
    free(data); free(offsets); free(zones); return error ? -1 : 0;
failed:
    free(offsets); free(zones); return -1;
}

static void free_songs(song_t *songs, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) afx_c_sf2_output_free(&songs[i].resolved);
    free(songs);
}

static source_t *find_source(source_t *sources, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; ++i) if (!strcmp(sources[i].name, name)) return sources + i;
    return NULL;
}

static map_t *find_map(map_t *maps, uint32_t count, const char *song, const afx_c_note_t *note) {
    unsigned bank = (unsigned)note->bank_msb * 128u + note->bank_lsb;
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(maps[i].song, song) && maps[i].midi_bank == bank && maps[i].midi_program == note->program)
            return maps + i;
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(maps[i].song, "*") && maps[i].midi_bank == bank && maps[i].midi_program == note->program)
            return maps + i;
    return NULL;
}

static int append_resolved(afx_c_sf2_output_t *to, afx_c_sf2_output_t *from) {
    uint32_t old_zones = to->zone_count, old_notes = to->note_count, old_owned = to->owned_count;
    if (!from->zone_names || from->zone_count > UINT16_MAX - old_zones || from->note_count > UINT32_MAX - old_notes ||
        from->owned_count > UINT32_MAX - old_owned) return -1;
    afx_c_zone_t *zones = realloc(to->zones, (size_t)(old_zones + from->zone_count) * sizeof(*zones));
    if (!zones) return -1;
    to->zones = zones;
    char (*names)[AFX_C_SAMPLE_NAME_BYTES] = realloc(to->zone_names,
                                                      (size_t)(old_zones + from->zone_count) * sizeof(*names));
    if (!names) return -1;
    to->zone_names = names;
    afx_c_note_t *notes = realloc(to->notes, (size_t)(old_notes + from->note_count) * sizeof(*notes));
    if (!notes) return -1;
    to->notes = notes;
    uint8_t **owned = realloc(to->owned_samples, (size_t)(old_owned + from->owned_count) * sizeof(*owned));
    if (!owned) return -1;
    to->owned_samples = owned;
    memcpy(to->zones + old_zones, from->zones, (size_t)from->zone_count * sizeof(*to->zones));
    memcpy(to->zone_names + old_zones, from->zone_names,
           (size_t)from->zone_count * sizeof(*to->zone_names));
    for (uint32_t i = 0; i < from->note_count; ++i) {
        if (!from->notes[i].setup_index || from->notes[i].setup_index > UINT16_MAX - old_zones) return -1;
        from->notes[i].setup_index = (uint16_t)(from->notes[i].setup_index + old_zones);
    }
    memcpy(to->notes + old_notes, from->notes, (size_t)from->note_count * sizeof(*to->notes));
    memcpy(to->owned_samples + old_owned, from->owned_samples, (size_t)from->owned_count * sizeof(*to->owned_samples));
    to->zone_count += from->zone_count; to->note_count += from->note_count; to->owned_count += from->owned_count;
    free(from->zones); free(from->zone_names); free(from->notes); free(from->owned_samples);
    *from = (afx_c_sf2_output_t){0};
    return 0;
}

static int parse_map(const char *path, source_t **out_sources, uint32_t *out_source_count,
                     map_t **out_maps, uint32_t *out_map_count, song_t **out_songs, uint32_t *out_song_count) {
    FILE *file = fopen(path, "r"); char line[1400];
    source_t *sources = NULL; map_t *maps = NULL; song_t *songs = NULL;
    uint32_t source_count = 0, map_count = 0, song_count = 0;
    if (!file) return -1;
    while (fgets(line, sizeof(line), file)) {
        char kind[16], c[128], f[16], channel[16];
        char *cursor = line; while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor == '#' || *cursor == '\n' || !*cursor) continue;
        if (sscanf(cursor, "%15s", kind) != 1) goto failed;
        if (!strcmp(kind, "source")) {
            source_t item = {0}; char resolved[1200];
            int fields = sscanf(cursor, "%15s %127s %1023s %15s", kind, item.name, item.path, channel);
            if (fields < 3 || fields > 4 || (fields == 4 && strcmp(channel, "stereo") &&
                strcmp(channel, "left") && strcmp(channel, "right")) ||
                map_count || song_count || find_source(sources, source_count, item.name) ||
                relative_path(path, item.path, resolved)) goto failed;
            item.channel = fields == 4 && !strcmp(channel, "left") ? AFX_C_SF2_LEFT :
                           fields == 4 && !strcmp(channel, "right") ? AFX_C_SF2_RIGHT : AFX_C_SF2_STEREO;
            strcpy(item.path, resolved);
            source_t *grown = realloc(sources, (size_t)(source_count + 1u) * sizeof(*sources));
            if (!grown) goto failed;
            sources = grown; sources[source_count++] = item;
        } else if (!strcmp(kind, "map")) {
            unsigned midi_bank, midi_program, sf2_bank, sf2_program; map_t item = {0};
            int fields = sscanf(cursor, "%15s %127s %u %u %127s %u %u %15s", kind, item.song, &midi_bank,
                                &midi_program, c, &sf2_bank, &sf2_program, f);
            if (fields != 8 || midi_bank > 16383 || midi_program > 127 ||
                song_count || sf2_bank > 16383 || sf2_program > 127 || afx_c_parse_sample_format(f, &item.format) ||
                !(item.source = find_source(sources, source_count, c))) goto failed;
            for (uint32_t i = 0; i < map_count; ++i)
                if (!strcmp(maps[i].song, item.song) && maps[i].midi_bank == midi_bank && maps[i].midi_program == midi_program)
                    goto failed;
            item.midi_bank = midi_bank; item.midi_program = midi_program;
            item.sf2_bank = sf2_bank; item.sf2_program = sf2_program;
            map_t *grown = realloc(maps, (size_t)(map_count + 1u) * sizeof(*maps));
            if (!grown) goto failed;
            maps = grown; maps[map_count++] = item;
        } else if (!strcmp(kind, "song")) {
            song_t item = {0}; char resolved[1200];
            if (sscanf(cursor, "%15s %127s %1023s", kind, item.name, item.midi) != 3 ||
                relative_path(path, item.midi, resolved)) goto failed;
            strcpy(item.midi, resolved);
            for (uint32_t i = 0; i < song_count; ++i) if (!strcmp(songs[i].name, item.name)) goto failed;
            song_t *grown = realloc(songs, (size_t)(song_count + 1u) * sizeof(*songs));
            if (!grown) goto failed;
            songs = grown; songs[song_count++] = item;
        } else goto failed;
    }
    fclose(file);
    if (!source_count || !map_count || !song_count) goto failed_no_file;
    *out_sources = sources; *out_source_count = source_count; *out_maps = maps; *out_map_count = map_count;
    *out_songs = songs; *out_song_count = song_count; return 0;
failed:
    fclose(file);
failed_no_file:
    free(sources); free(maps); free_songs(songs, song_count); return -1;
}

static int resolve_song(song_t *song, map_t *maps, uint32_t map_count) {
    uint8_t *midi = NULL; uint32_t midi_bytes = 0;
    afx_c_note_t *notes = NULL; uint32_t note_count = 0;
    if (read_file(song->midi, &midi, &midi_bytes) || afx_c_midi_notes(midi, midi_bytes, 1000, &notes, &note_count)) goto failed;
    for (uint32_t map_index = 0; map_index < map_count; ++map_index) {
        uint32_t matching = 0;
        for (uint32_t i = 0; i < note_count; ++i)
            if (find_map(maps, map_count, song->name, notes + i) == maps + map_index) ++matching;
        if (!matching) continue;
        afx_c_note_t *selected = malloc((size_t)matching * sizeof(*selected));
        if (!selected) goto failed;
        uint32_t at = 0;
        for (uint32_t i = 0; i < note_count; ++i)
            if (find_map(maps, map_count, song->name, notes + i) == maps + map_index) {
            selected[at] = notes[i]; selected[at].bank_msb = maps[map_index].sf2_bank / 128u;
            selected[at].bank_lsb = maps[map_index].sf2_bank % 128u; selected[at++].program = maps[map_index].sf2_program;
        }
        afx_c_sf2_output_t resolved = {0};
        int resolve_result = afx_c_sf2_resolve(maps[map_index].source->path, selected, matching,
                                               maps[map_index].format, maps[map_index].source->channel, &resolved);
        int failed_resolve = resolve_result || append_resolved(&song->resolved, &resolved);
        free(selected); afx_c_sf2_output_free(&resolved);
        if (failed_resolve) goto failed;
    }
    for (uint32_t i = 0; i < note_count; ++i) if (!find_map(maps, map_count, song->name, notes + i)) goto failed;
    free(midi); free(notes); return song->resolved.note_count ? 0 : -1;
failed:
    free(midi); free(notes); return -1;
}

typedef struct { unsigned bank, program; } program_t;

static int collect_programs(const char *path, program_t **programs, uint32_t *count) {
    uint8_t *midi = NULL; uint32_t midi_bytes = 0;
    afx_c_note_t *notes = NULL; uint32_t note_count = 0;
    if (read_file(path, &midi, &midi_bytes) || afx_c_midi_notes(midi, midi_bytes, 1000, &notes, &note_count)) goto failed;
    for (uint32_t i = 0; i < note_count; ++i) {
        program_t item = {(unsigned)notes[i].bank_msb * 128u + notes[i].bank_lsb, notes[i].program};
        uint32_t at = 0;
        while (at < *count && ((*programs)[at].bank != item.bank || (*programs)[at].program != item.program)) ++at;
        if (at == *count) {
            program_t *grown = realloc(*programs, (size_t)(at + 1u) * sizeof(*grown));
            if (!grown) goto failed;
            *programs = grown; (*programs)[(*count)++] = item;
        }
    }
    free(midi); free(notes); return note_count ? 0 : -1;
failed:
    free(midi); free(notes); return -1;
}

static int create_map(int argc, char **argv) {
    /* argv: --create-map output.afbm soundfont.sf2 name midi [name midi ...] */
    if (argc < 6 || (argc - 4) & 1) return -1;
    program_t *programs = NULL; uint32_t program_count = 0;
    for (int i = 4; i < argc; i += 2)
        if (collect_programs(argv[i + 1], &programs, &program_count)) { free(programs); return -1; }
    char sf2_path[1200];
    if (map_relative_path(argv[2], argv[3], sf2_path)) { free(programs); return -1; }
    FILE *file = fopen(argv[2], "w");
    if (!file) { free(programs); return -1; }
    int failed = fprintf(file,
        "# AICAflow bank map. Edit source/map lines to combine SoundFonts or tune formats.\n"
        "# source <name> <soundfont.sf2> [stereo|left|right]\n"
        "# map <song|*> <midi-bank> <midi-program> <source> <sf2-bank> <sf2-program> <pcm16|pcm8|adpcm|auto>\n"
        "# song <output-basename> <source.mid>\n\n"
        "source default %s\n\n", sf2_path) < 0;
    for (uint32_t i = 0; !failed && i < program_count; ++i)
        failed = fprintf(file, "map * %u %u default %u %u auto\n", programs[i].bank, programs[i].program,
                         programs[i].bank, programs[i].program) < 0;
    for (int i = 4; !failed && i < argc; i += 2) {
        char midi_path[1200];
        if (map_relative_path(argv[2], argv[i + 1], midi_path)) failed = 1;
        else failed = fprintf(file, "song %s %s\n", argv[i], midi_path) < 0;
    }
    failed |= fclose(file);
    free(programs); return failed ? -1 : 0;
}

static int build_per_song(const char *map_path, const char *directory) {
    source_t *sources = NULL; map_t *maps = NULL; song_t *songs = NULL;
    uint32_t source_count = 0, map_count = 0, song_count = 0;
    if (parse_map(map_path, &sources, &source_count, &maps, &map_count, &songs, &song_count)) goto failed;
    for (uint32_t song = 0; song < song_count; ++song) {
        afx_c_output_t out = {0};
        char afb[4096], afx[4096], afc[4096], afv[4096], afi[4096], named_afi[4096];
        if (resolve_song(songs + song, maps, map_count) ||
            afx_c_compile_zones(songs[song].resolved.notes, songs[song].resolved.note_count, 1000,
                                songs[song].resolved.zones, songs[song].resolved.zone_count, &out)) goto failed;
        if (out.afb_bytes < AFX_BANK_HEADER_BYTES || out.afb_bytes - AFX_BANK_HEADER_BYTES > AFX_ASSET_MAX ||
            snprintf(afb, sizeof(afb), "%s/%s.afb", directory, songs[song].name) >= (int)sizeof(afb) ||
            flow_paths(directory, songs[song].name, afx, afc, afv) ||
            index_paths(afb, afi, named_afi) ||
            write_file(afb, out.afb, out.afb_bytes) || write_file(afx, out.afx, out.afx_bytes) ||
            write_file(afc, out.afc, out.afc_bytes) || write_file(afv, out.afv, out.afv_bytes) ||
            write_afi(afi, out.afb, out.afb_bytes, songs[song].resolved.zones,
                      songs[song].resolved.zone_count, songs[song].resolved.zone_names, 0) ||
            write_afi(named_afi, out.afb, out.afb_bytes, songs[song].resolved.zones,
                      songs[song].resolved.zone_count, songs[song].resolved.zone_names, 1)) {
            afx_c_output_free(&out); goto failed;
        }
        afx_c_output_free(&out);
    }
    free(sources); free(maps); free_songs(songs, song_count); return 0;
failed:
    free(sources); free(maps); free_songs(songs, song_count);
    fprintf(stderr, "cannot build per-song banks from %s\n", map_path); return 1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--create-map")) {
        if (create_map(argc, argv))
            return fprintf(stderr, "usage: %s --create-map library.afbm bank.sf2 name song.mid [name song.mid ...]\n", argv[0]), 2;
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "--per-song")) return build_per_song(argv[2], argv[3]);
    if (argc != 4) return fprintf(stderr, "usage: %s library.afbm output-dir bank.afb\n", argv[0]), 2;
    source_t *sources = NULL; map_t *maps = NULL; song_t *songs = NULL;
    uint32_t source_count = 0, map_count = 0, song_count = 0, zone_count = 0;
    afx_c_zone_t *zones = NULL;
    char (*zone_names)[AFX_C_SAMPLE_NAME_BYTES] = NULL;
    if (parse_map(argv[1], &sources, &source_count, &maps, &map_count, &songs, &song_count)) goto failed;
    for (uint32_t song = 0; song < song_count; ++song) {
        if (resolve_song(songs + song, maps, map_count) || songs[song].resolved.zone_count > UINT32_MAX - zone_count ||
            songs[song].resolved.note_count > UINT16_MAX) goto failed;
        afx_c_zone_t *grown = realloc(zones, (size_t)(zone_count + songs[song].resolved.zone_count) * sizeof(*zones));
        if (!grown) goto failed;
        zones = grown;
        char (*grown_names)[AFX_C_SAMPLE_NAME_BYTES] = realloc(zone_names,
            (size_t)(zone_count + songs[song].resolved.zone_count) * sizeof(*grown_names));
        if (!grown_names) goto failed;
        zone_names = grown_names;
        memcpy(zones + zone_count, songs[song].resolved.zones,
               (size_t)songs[song].resolved.zone_count * sizeof(*zones));
        memcpy(zone_names + zone_count, songs[song].resolved.zone_names,
               (size_t)songs[song].resolved.zone_count * sizeof(*zone_names));
        for (uint32_t note = 0; note < songs[song].resolved.note_count; ++note) {
            uint32_t setup = zone_count + songs[song].resolved.notes[note].setup_index;
            if (!setup || setup > UINT16_MAX) goto failed;
            songs[song].resolved.notes[note].setup_index = (uint16_t)setup;
        }
        zone_count += songs[song].resolved.zone_count;
    }
    if (!zone_count) goto failed;
    uint32_t bank_low = 0, bank_high = 0;
    for (uint32_t song = 0; song < song_count; ++song) {
        afx_c_output_t out;
        if (afx_c_compile_zones(songs[song].resolved.notes, songs[song].resolved.note_count, 1000,
                                zones, zone_count, &out)) goto failed;
        if (out.afb_bytes < AFX_BANK_HEADER_BYTES || out.afb_bytes - AFX_BANK_HEADER_BYTES > AFX_ASSET_MAX) {
            afx_c_output_free(&out); goto failed;
        }
        if (!song) {
            bank_low = afx_read32(out.afb + 8); bank_high = afx_read32(out.afb + 12);
            char afi[4096], named_afi[4096];
            if (write_file(argv[3], out.afb, out.afb_bytes) || index_paths(argv[3], afi, named_afi) ||
                write_afi(afi, out.afb, out.afb_bytes, zones, zone_count, zone_names, 0) ||
                write_afi(named_afi, out.afb, out.afb_bytes, zones, zone_count, zone_names, 1)) {
                afx_c_output_free(&out); goto failed;
            }
        }
        if (afx_read32(out.afx + 40) != bank_low || afx_read32(out.afx + 44) != bank_high) {
            afx_c_output_free(&out); goto failed;
        }
        char afx[4096], afc[4096], afv[4096];
        int error = flow_paths(argv[2], songs[song].name, afx, afc, afv) ||
                    write_file(afx, out.afx, out.afx_bytes) || write_file(afc, out.afc, out.afc_bytes) ||
                    write_file(afv, out.afv, out.afv_bytes);
        afx_c_output_free(&out);
        if (error) goto failed;
    }
    free(sources); free(maps); free(zone_names); free(zones); free_songs(songs, song_count); return 0;
failed:
    free(sources); free(maps); free(zone_names); free(zones); free_songs(songs, song_count);
    fprintf(stderr, "cannot build shared bank from %s\n", argv[1]); return 1;
}
