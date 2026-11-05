/* afx_scene: plans AFS scene files from a manifest and checks that a permanent
 * scene plus each level scene (two levels, with overlapping transitions) fit
 * the AICA asset arena of a firmware image.
 *
 *     afx_scene manifest.afsm
 *
 * The manifest is line-based; # starts a comment. Paths are relative to the
 * manifest's directory. Entries belong to the scene line before them.
 *
 *     firmware aicaflow.drv        the image the program boots; it fixes the arena
 *     overlap                      the next level loads before the last is released
 *     permanent core out/core.afs  at most one; levels bind its banks
 *     level forest out/forest.afs
 *     bank core core.afb
 *     flow theme theme.afx
 *     slot song 131072             room for a bank and a flow loaded later
 *     ring 3                       the DSP delay ring, 16 KiB << RBL
 *
 * Every block is laid out with the runtime's rounding: 32 bytes per entry, a
 * ring first on a 2 KiB boundary. The AFS layout is checked with the same
 * validator the loader uses. Nothing is written unless everything fits. */
#include <aicaflow/codec.h>
#include <aicaflow/limits.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAME_BYTES 64u
#define PATH_BYTES 1200u

typedef struct {
    uint32_t kind;
    char name[NAME_BYTES];
    uint8_t *file;
    uint32_t file_size, offset, bytes, ticks;
    uint32_t id_low, id_high; /* A bank's identity. */
    uint32_t needs[AFX_MAX_FLOW_BANKS][2], need_count; /* A flow's bank identities. */
} entry_t;
typedef struct {
    char name[NAME_BYTES], out[PATH_BYTES];
    bool permanent;
    entry_t *entries;
    uint32_t entry_count, block_bytes, align;
} scene_t;

static uint32_t align_to(uint32_t value, uint32_t align) { return (value + align - 1u) & ~(align - 1u); }

static int read_file(const char *path, uint8_t **out_data, uint32_t *out_bytes) {
    FILE *file = fopen(path, "rb"); long bytes; uint8_t *data;
    if (!file || fseek(file, 0, SEEK_END) || (bytes = ftell(file)) < 1 ||
        (uint64_t)bytes > UINT32_MAX || fseek(file, 0, SEEK_SET)) goto failed;
    data = malloc((size_t)bytes);
    if (!data || fread(data, 1, (size_t)bytes, file) != (size_t)bytes) { free(data); goto failed; }
    fclose(file); *out_data = data; *out_bytes = (uint32_t)bytes; return 0;
failed:
    if (file) fclose(file);
    return -1;
}

static int write_file(const char *path, const uint8_t *data, uint32_t bytes) {
    FILE *file = fopen(path, "wb");
    int failed = !file || fwrite(data, 1, bytes, file) != bytes;
    if (file && fclose(file)) failed = 1;
    return failed ? -1 : 0;
}

static int relative_path(const char *manifest, const char *path, char out[PATH_BYTES]) {
    const char *slash = strrchr(manifest, '/');
    size_t dir = slash ? (size_t)(slash - manifest) : 0;
    if (path[0] == '/' || !dir) return snprintf(out, PATH_BYTES, "%s", path) >= (int)PATH_BYTES ? -1 : 0;
    if (dir + 1u + strlen(path) >= PATH_BYTES) return -1;
    memcpy(out, manifest, dir); out[dir] = '/'; strcpy(out + dir + 1u, path); return 0;
}

static uint32_t fnv(const uint8_t *data, size_t bytes, uint32_t hash) {
    for (size_t i = 0; i < bytes; ++i) hash = (hash ^ data[i]) * 16777619u;
    return hash;
}

/* An AFB as the loader decodes it: identity, then one 32-byte aligned payload. */
static int bank_entry(entry_t *e) {
    const uint8_t *p = e->file;
    if (e->file_size < AFX_BANK_HEADER_BYTES || afx_read32(p) != AFX_BANK_MAGIC ||
        afx_read32(p + 4) != AFX_BANK_VERSION) return -1;
    uint32_t offset = afx_read32(p + 16), bytes = afx_read32(p + 20);
    e->id_low = afx_read32(p + 8); e->id_high = afx_read32(p + 12);
    if (!(e->id_low || e->id_high) || afx_read32(p + 24) != e->file_size || offset < AFX_BANK_HEADER_BYTES ||
        (offset & 31u) || !bytes || offset > e->file_size || bytes != e->file_size - offset) return -1;
    e->bytes = align_to(bytes, 32);
    return 0;
}
static int flow_entry(entry_t *e) {
    afx_file_header_t header;
    uint64_t ticks;
    uint32_t num, den;
    if (afx_file_validate(e->file, e->file_size, &header) ||
        afx_flow_duration(e->file, e->file_size, &ticks, &num, &den) || ticks > UINT32_MAX) return -1;
    e->ticks = (uint32_t)ticks;
    e->bytes = align_to(header.image_size, 32);
    e->need_count = afx_file_bank_count(e->file, &header);
    for (uint32_t slot = 0; slot < e->need_count; ++slot)
        afx_file_bank_identity(e->file, &header, slot, &e->needs[slot][0], &e->needs[slot][1]);
    return 0;
}

static const entry_t *find_bank(const scene_t *s, uint32_t low, uint32_t high) {
    for (uint32_t i = 0; s && i < s->entry_count; ++i)
        if (s->entries[i].kind == AFX_SCENE_BANK && s->entries[i].id_low == low && s->entries[i].id_high == high)
            return &s->entries[i];
    return NULL;
}

/* The ring first, on the block's 2 KiB boundary; then the rest in manifest order. */
static void lay_out(scene_t *s) {
    uint32_t at = 0;
    s->align = 32;
    for (uint32_t i = 0; i < s->entry_count; ++i)
        if (s->entries[i].kind == AFX_SCENE_RING) {
            s->entries[i].offset = 0;
            at = s->entries[i].bytes;
            s->align = AFX_SCENE_RING_ALIGN;
        }
    for (uint32_t i = 0; i < s->entry_count; ++i)
        if (s->entries[i].kind != AFX_SCENE_RING) {
            s->entries[i].offset = at;
            at += s->entries[i].bytes;
        }
    s->block_bytes = at;
}

/* The AFS bytes of a laid-out scene, checked by the loader's validator. */
static uint8_t *encode(const scene_t *s, uint32_t *out_bytes) {
    uint32_t at = AFX_SCENE_HEADER_BYTES + s->entry_count * AFX_SCENE_ENTRY_BYTES;
    for (uint32_t i = 0; i < s->entry_count; ++i) at += (uint32_t)strlen(s->entries[i].name) + 1u;
    for (uint32_t i = 0; i < s->entry_count; ++i)
        if (s->entries[i].file) at = align_to(at, 32) + s->entries[i].file_size;
    uint8_t *out = calloc(1, at);
    if (!out) return NULL;
    uint32_t names = AFX_SCENE_HEADER_BYTES + s->entry_count * AFX_SCENE_ENTRY_BYTES, files;
    for (uint32_t i = 0; i < s->entry_count; ++i) {
        const entry_t *e = &s->entries[i];
        uint8_t *entry = out + AFX_SCENE_HEADER_BYTES + i * AFX_SCENE_ENTRY_BYTES;
        afx_write32(entry, e->kind);
        afx_write32(entry + 4, names);
        memcpy(out + names, e->name, strlen(e->name) + 1u);
        names += (uint32_t)strlen(e->name) + 1u;
        afx_write32(entry + 16, e->offset);
        afx_write32(entry + 20, e->bytes);
        afx_write32(entry + 24, e->ticks);
    }
    files = names;
    for (uint32_t i = 0; i < s->entry_count; ++i) {
        const entry_t *e = &s->entries[i];
        if (!e->file) continue;
        uint8_t *entry = out + AFX_SCENE_HEADER_BYTES + i * AFX_SCENE_ENTRY_BYTES;
        files = align_to(files, 32);
        memcpy(out + files, e->file, e->file_size);
        afx_write32(entry + 8, files);
        afx_write32(entry + 12, e->file_size);
        files += e->file_size;
    }
    uint32_t low = fnv((const uint8_t *)s->name, strlen(s->name), 2166136261u);
    uint32_t high = fnv(out + AFX_SCENE_HEADER_BYTES, at - AFX_SCENE_HEADER_BYTES, 2166136261u ^ 0x9e3779b9u);
    afx_write32(out, AFX_SCENE_MAGIC); afx_write32(out + 4, AFX_SCENE_VERSION);
    afx_write32(out + 8, low); afx_write32(out + 12, high ? high : 1u);
    afx_write32(out + 16, at); afx_write32(out + 20, s->block_bytes);
    afx_write32(out + 24, s->entry_count);
    if (afx_scene_validate(out, at, NULL)) { free(out); return NULL; }
    *out_bytes = at;
    return out;
}

/* A block's worst-case footprint after other blocks: its bytes and the
 * padding its alignment may cost. */
static uint32_t footprint(const scene_t *s) { return s->block_bytes + s->align - 32u; }

static int parse(const char *path, char firmware[PATH_BYTES], bool *overlap, scene_t **out, uint32_t *count) {
    FILE *file = fopen(path, "r");
    char line[4096];
    unsigned number = 0;
    scene_t *scenes = NULL;
    uint32_t n = 0;
    if (!file) return fprintf(stderr, "%s: cannot read\n", path), -1;
    firmware[0] = 0;
    while (fgets(line, sizeof(line), file)) {
        ++number;
        char *hash = strchr(line, '#'), *words[4] = {0};
        if (hash) *hash = 0;
        unsigned w = 0;
        for (char *word = strtok(line, " \t\r\n"); word; word = strtok(NULL, " \t\r\n"))
            if (w < 4) words[w++] = word; else { w = 5; break; }
        if (!w) continue;
        const char *d = words[0];
        scene_t *s = n ? &scenes[n - 1u] : NULL;
        bool scene_line = !strcmp(d, "permanent") || !strcmp(d, "level");
        bool entry_line = !strcmp(d, "bank") || !strcmp(d, "flow") || !strcmp(d, "slot");
        if (!strcmp(d, "firmware") && w == 2) {
            if (relative_path(path, words[1], firmware)) goto syntax;
        } else if (!strcmp(d, "overlap") && w == 1) {
            *overlap = true;
        } else if (scene_line && w == 3) {
            scene_t *grown = realloc(scenes, (n + 1u) * sizeof(*scenes));
            if (!grown) goto syntax;
            scenes = grown;
            s = &scenes[n++];
            memset(s, 0, sizeof(*s));
            s->permanent = !strcmp(d, "permanent");
            if (strlen(words[1]) >= NAME_BYTES || relative_path(path, words[2], s->out)) goto syntax;
            strcpy(s->name, words[1]);
        } else if ((entry_line && w == 3) || (!strcmp(d, "ring") && w == 2)) {
            if (!s) goto syntax;
            entry_t *grown = realloc(s->entries, (s->entry_count + 1u) * sizeof(*s->entries));
            if (!grown) goto syntax;
            s->entries = grown;
            entry_t *e = &s->entries[s->entry_count++];
            memset(e, 0, sizeof(*e));
            char *end = NULL;
            if (!strcmp(d, "ring")) {
                unsigned long rbl = strtoul(words[1], &end, 10);
                if (*end || rbl > 3) goto syntax;
                e->kind = AFX_SCENE_RING;
                strcpy(e->name, "ring");
                e->bytes = AFX_SCENE_RING_MIN_BYTES << rbl;
                continue;
            }
            if (strlen(words[1]) >= NAME_BYTES) goto syntax;
            strcpy(e->name, words[1]);
            if (!strcmp(d, "slot")) {
                unsigned long bytes = strtoul(words[2], &end, 0);
                if (*end || !bytes || bytes > AFX_TARGET_MAX_BANK_BYTES) goto syntax;
                e->kind = AFX_SCENE_SLOT;
                e->bytes = align_to((uint32_t)bytes, 32);
                continue;
            }
            char input[PATH_BYTES];
            e->kind = !strcmp(d, "bank") ? AFX_SCENE_BANK : AFX_SCENE_FLOW;
            if (relative_path(path, words[2], input) || read_file(input, &e->file, &e->file_size)) {
                fprintf(stderr, "%s:%u: cannot read %s\n", path, number, words[2]);
                goto failed;
            }
            if (e->kind == AFX_SCENE_BANK ? bank_entry(e) : flow_entry(e)) {
                fprintf(stderr, "%s:%u: %s is not a valid %s\n", path, number, words[2],
                        e->kind == AFX_SCENE_BANK ? "AFB bank" : "AFX flow");
                goto failed;
            }
        } else {
            goto syntax;
        }
    }
    fclose(file);
    *out = scenes; *count = n;
    return 0;
syntax:
    fprintf(stderr, "%s:%u: cannot use this line\n", path, number);
failed:
    fclose(file);
    *out = scenes; *count = n;
    return -1;
}

/* Names, bank identities and bindings: what the loader would refuse. */
static int check(const scene_t *scenes, uint32_t count, const scene_t **permanent) {
    *permanent = NULL;
    for (uint32_t i = 0; i < count; ++i) {
        const scene_t *s = &scenes[i];
        if (s->permanent && *permanent)
            return fprintf(stderr, "%s: only one permanent scene\n", s->name), -1;
        if (s->permanent) *permanent = s;
        if (!s->entry_count) return fprintf(stderr, "%s: no entries\n", s->name), -1;
        for (uint32_t j = 0; j < count; ++j)
            if (j != i && !strcmp(s->name, scenes[j].name))
                return fprintf(stderr, "%s: two scenes with this name\n", s->name), -1;
        for (uint32_t a = 0; a < s->entry_count; ++a)
            for (uint32_t b = 0; b < a; ++b) {
                const entry_t *x = &s->entries[a], *y = &s->entries[b];
                if (x->kind == AFX_SCENE_RING && y->kind == AFX_SCENE_RING)
                    return fprintf(stderr, "%s: two rings\n", s->name), -1;
                if (!strcmp(x->name, y->name))
                    return fprintf(stderr, "%s: two entries named %s\n", s->name, x->name), -1;
                if (x->kind == AFX_SCENE_BANK && y->kind == AFX_SCENE_BANK &&
                    x->id_low == y->id_low && x->id_high == y->id_high)
                    return fprintf(stderr, "%s: banks %s and %s have one identity\n", s->name, y->name, x->name), -1;
            }
    }
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = 0; j < scenes[i].entry_count; ++j) {
            const entry_t *e = &scenes[i].entries[j];
            for (uint32_t slot = 0; slot < e->need_count; ++slot)
                if (!find_bank(&scenes[i], e->needs[slot][0], e->needs[slot][1]) &&
                    (scenes[i].permanent || !find_bank(*permanent, e->needs[slot][0], e->needs[slot][1])))
                    return fprintf(stderr, "%s: flow %s needs bank %08x%08x in slot %u, which %s\n",
                                   scenes[i].name, e->name, e->needs[slot][1], e->needs[slot][0], slot,
                                   scenes[i].permanent ? "the scene lacks" : "neither it nor the permanent scene has"), -1;
        }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return fprintf(stderr, "usage: %s manifest.afsm\n", argv[0]), 2;
    char firmware_path[PATH_BYTES];
    bool overlap = false;
    scene_t *scenes = NULL;
    uint32_t count = 0;
    const scene_t *permanent = NULL;
    uint8_t *firmware = NULL;
    uint32_t firmware_bytes = 0;
    int result = 1;
    if (parse(argv[1], firmware_path, &overlap, &scenes, &count) || check(scenes, count, &permanent)) goto done;
    if (!firmware_path[0] || read_file(firmware_path, &firmware, &firmware_bytes)) {
        fprintf(stderr, "%s: needs a readable firmware line\n", argv[1]);
        goto done;
    }
    /* The firmware validator requires the asset arena to start at the image's
     * size rounded up to 32 bytes; it ends at the fixed asset ceiling. */
    uint32_t base = align_to(firmware_bytes, 32), limit = AFX_TARGET_MAX_BANK_BYTES;
    if (base >= limit) { fprintf(stderr, "%s: firmware leaves no arena\n", firmware_path); goto done; }
    uint32_t arena = limit - base;
    printf("arena %u bytes, 0x%06x-0x%06x\n", arena, base, limit);

    /* The permanent scene loads first, into an empty arena; a level after it
     * may pay its alignment, and so may the second level of a transition. */
    uint32_t resident = 0, largest = 0, second = 0;
    const char *largest_name = NULL, *second_name = NULL;
    bool fits = true;
    for (uint32_t i = 0; i < count; ++i) lay_out(&scenes[i]);
    if (permanent) {
        resident = align_to(base, permanent->align) - base + permanent->block_bytes;
        printf("permanent %s %u bytes, %u entries, block at 0x%06x\n", permanent->name,
               permanent->block_bytes, permanent->entry_count, align_to(base, permanent->align));
        if (resident > arena) fits = false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const scene_t *s = &scenes[i];
        if (s->permanent) continue;
        uint64_t need = (uint64_t)resident + footprint(s);
        printf("level %s %u bytes, %u entries, headroom %lld\n", s->name, s->block_bytes, s->entry_count,
               (long long)arena - (long long)need);
        if (need > arena) fits = false;
        if (footprint(s) > largest) {
            second = largest; second_name = largest_name;
            largest = footprint(s); largest_name = s->name;
        } else if (footprint(s) > second) {
            second = footprint(s); second_name = s->name;
        }
    }
    if (overlap && second_name) {
        uint64_t need = (uint64_t)resident + largest + second;
        printf("overlap %s + %s, headroom %lld\n", largest_name, second_name, (long long)arena - (long long)need);
        if (need > arena) fits = false;
    }
    if (!fits) { fprintf(stderr, "%s: the scenes do not fit the arena\n", argv[1]); goto done; }

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t bytes = 0;
        uint8_t *afs = encode(&scenes[i], &bytes);
        if (!afs || write_file(scenes[i].out, afs, bytes)) {
            fprintf(stderr, "%s: cannot write %s\n", scenes[i].name, scenes[i].out);
            free(afs);
            goto done;
        }
        free(afs);
    }
    result = 0;
done:
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t j = 0; j < scenes[i].entry_count; ++j) free(scenes[i].entries[j].file);
        free(scenes[i].entries);
    }
    free(scenes);
    free(firmware);
    return result;
}
