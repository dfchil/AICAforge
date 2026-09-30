#include <aicaflow/codec.h>

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A deliberately small, fixed-shape JSON reader for the scene portion of an
 * .afp.  Profiles are offline inputs, but binding them to the exact base AFX
 * still prevents a stale artistic decision silently changing another song. */
typedef struct {
    char sha256[65];
    char preset[32];
    uint8_t send;
} profile_t;

typedef struct { uint32_t state[8]; uint64_t bits; uint8_t block[64]; unsigned used; } sha256_t;

static const uint32_t sha256_initial[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
};
static const uint32_t sha256_k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};
static uint32_t rotr(uint32_t value, unsigned bits) { return value >> bits | value << (32u - bits); }
static void sha256_block(sha256_t *ctx, const uint8_t block[64]) {
    uint32_t words[64], a, b, c, d, e, f, g, h;
    for (unsigned i = 0; i < 16; ++i)
        words[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
                   (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(words[i - 15], 7) ^ rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
        uint32_t s1 = rotr(words[i - 2], 17) ^ rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t s1=rotr(e,6)^rotr(e,11)^rotr(e,25), choose=(e&f)^(~e&g);
        uint32_t s0=rotr(a,2)^rotr(a,13)^rotr(a,22), majority=(a&b)^(a&c)^(b&c);
        uint32_t next1=h+s1+choose+sha256_k[i]+words[i], next2=s0+majority;
        h=g; g=f; f=e; e=d+next1; d=c; c=b; b=a; a=next1+next2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}
static void sha256_init(sha256_t *ctx) { memcpy(ctx->state, sha256_initial, sizeof(ctx->state)); ctx->bits=0; ctx->used=0; }
static void sha256_update(sha256_t *ctx, const uint8_t *data, size_t bytes) {
    ctx->bits += (uint64_t)bytes * 8u;
    while (bytes) {
        size_t copy = 64u - ctx->used; if (copy > bytes) copy = bytes;
        memcpy(ctx->block + ctx->used, data, copy); ctx->used += (unsigned)copy; data += copy; bytes -= copy;
        if (ctx->used == 64) { sha256_block(ctx, ctx->block); ctx->used = 0; }
    }
}
static void sha256_final(sha256_t *ctx, char hex[65]) {
    static const char digits[] = "0123456789abcdef";
    ctx->block[ctx->used++] = 0x80;
    if (ctx->used > 56) { memset(ctx->block + ctx->used, 0, 64 - ctx->used); sha256_block(ctx, ctx->block); ctx->used = 0; }
    memset(ctx->block + ctx->used, 0, 56 - ctx->used);
    for (unsigned i = 0; i < 8; ++i) ctx->block[56 + i] = (uint8_t)(ctx->bits >> (56 - 8 * i));
    sha256_block(ctx, ctx->block);
    for (unsigned i = 0; i < 8; ++i) for (unsigned j = 0; j < 4; ++j) {
        uint8_t byte = (uint8_t)(ctx->state[i] >> (24 - j * 8));
        hex[(i * 4 + j) * 2] = digits[byte >> 4]; hex[(i * 4 + j) * 2 + 1] = digits[byte & 15];
    }
    hex[64] = 0;
}
static void digest(const uint8_t *data, uint32_t bytes, char hex[65]) {
    sha256_t ctx; sha256_init(&ctx); sha256_update(&ctx, data, bytes); sha256_final(&ctx, hex);
}

static int read_file(const char *path, uint8_t **out, uint32_t *out_bytes) {
    FILE *file = fopen(path, "rb"); long length; uint8_t *data;
    if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) < 1 || (uint64_t)length > UINT32_MAX ||
        fseek(file, 0, SEEK_SET)) goto failed;
    data = malloc((size_t)length + 1u);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) { free(data); goto failed; }
    fclose(file); data[length] = 0; *out = data; *out_bytes = (uint32_t)length; return 0;
failed:
    if (file) fclose(file); return -1;
}
static int write_file(const char *path, const uint8_t *data, uint32_t bytes) {
    FILE *file = fopen(path, "wb"); int failed = !file || fwrite(data, 1, bytes, file) != bytes;
    if (file && fclose(file)) failed = 1; return failed ? -1 : 0;
}
static const char *skip_space(const char *p) { while (*p && isspace((unsigned char)*p)) ++p; return p; }
static int find_value(const char *json, const char *key, const char **out) {
    char needle[80]; const char *match = NULL, *cursor = json;
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) >= (int)sizeof(needle)) return -1;
    while ((cursor = strstr(cursor, needle))) {
        const char *value = skip_space(cursor + strlen(needle));
        if (*value != ':') return -1;
        if (match) return -1;
        match = skip_space(value + 1); cursor += strlen(needle);
    }
    if (!match) return -1; *out = match; return 0;
}
static int json_string(const char *json, const char *key, char *out, size_t capacity) {
    const char *p; size_t used = 0;
    if (find_value(json, key, &p) || *p++ != '"') return -1;
    while (*p && *p != '"') {
        if (*p == '\\' || used + 1 >= capacity) return -1;
        out[used++] = *p++;
    }
    if (*p != '"') return -1; out[used] = 0; return 0;
}
static int json_unsigned(const char *json, const char *key, unsigned *out) {
    const char *p; char *end; unsigned long value;
    if (find_value(json, key, &p) || !isdigit((unsigned char)*p)) return -1;
    value = strtoul(p, &end, 10);
    if ((*end && !strchr(",}\t\r\n ", *end)) || value > UINT_MAX) return -1;
    *out = (unsigned)value; return 0;
}
static int valid_preset(const char *preset) {
    return !strcmp(preset, "dry") || !strcmp(preset, "room") || !strcmp(preset, "room_warm") ||
           !strcmp(preset, "room_large");
}
static int profile_read(const char *path, const uint8_t *base, uint32_t base_bytes, profile_t *out) {
    uint8_t *json; uint32_t bytes; char format[32] = {0}, sha[65] = {0}; unsigned version = 0, abi = 0, send = 0;
    if (read_file(path, &json, &bytes)) return -1;
    int failed = json_string((char *)json, "format", format, sizeof(format)) ||
                 json_unsigned((char *)json, "version", &version) ||
                 json_unsigned((char *)json, "abi", &abi) ||
                 json_string((char *)json, "canonical_sha256", sha, sizeof(sha)) ||
                 json_string((char *)json, "preset", out->preset, sizeof(out->preset)) ||
                 json_unsigned((char *)json, "send", &send) || strcmp(format, "aicaflow.afp") ||
                 version != 1 || abi != AFX_FILE_VERSION || strlen(sha) != 64 || send > 255 || !valid_preset(out->preset);
    digest(base, base_bytes, out->sha256);
    if (!failed && strcmp(sha, out->sha256)) failed = 1;
    if (!failed && ((!strcmp(out->preset, "dry")) != (send == 0))) failed = 1;
    out->send = (uint8_t)send;
    free(json); return failed ? -1 : 0;
}
static int profile_write(const char *path, const uint8_t *base, uint32_t bytes, const char *preset, unsigned send) {
    profile_t profile; afx_file_header_t header;
    if (!valid_preset(preset) || send > 255 || ((!strcmp(preset, "dry")) != (send == 0))) return -1;
    if (afx_file_validate(base, bytes, &header)) return -1;
    digest(base, bytes, profile.sha256);
    FILE *file = fopen(path, "w");
    if (!file) return -1;
    int failed = fprintf(file,
        "{\n  \"format\": \"aicaflow.afp\",\n  \"version\": 1,\n"
        "  \"base\": { \"abi\": %u, \"canonical_sha256\": \"%s\" },\n"
        "  \"dsp\": { \"preset\": \"%s\", \"send\": %u },\n"
        "  \"templates\": {\n"
        "    \"all-notes\": { \"label\": \"All notes\", \"parameters\": { \"dsp_send\": %u } }\n"
        "  },\n  \"assignments\": [\n",
        AFX_FILE_VERSION, profile.sha256, preset, send, send) < 0;
    uint32_t tick = 0, ordinal = 0, ordinal_tick = UINT32_MAX;
    int first_note = 1;
    const uint8_t *image = base + header.image_offset;
    for (uint32_t offset = header.stream_offset, end = header.stream_offset + header.stream_size;
         !failed && offset < end;) {
        afx_event_t event;
        if (afx_decode_event(image + offset, end - offset, &event)) { failed = 1; break; }
        if (event.opcode >= AFX_OP_WAIT8 && event.opcode <= AFX_OP_WAIT32) tick += event.wait;
        if (event.opcode == AFX_OP_NOTE) {
            if (ordinal_tick != tick) { ordinal_tick = tick; ordinal = 0; }
            const char *separator = first_note ? "" : ",\n";
            failed = fprintf(file,
                "%s    { \"event\": { \"kind\": \"note\", \"tick\": %u, \"ordinal\": %u, \"channel\": %u }, \"template\": \"all-notes\" }",
                separator, tick, ordinal, event.channel) < 0;
            ++ordinal;
            first_note = 0;
        }
        offset += event.bytes;
    }
    failed |= fprintf(file, "  ]\n}\n") < 0;
    failed |= fclose(file); return failed ? -1 : 0;
}
static uint32_t control_id(const uint8_t *image, uint32_t bytes) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < bytes; ++i) hash = (hash ^ image[i]) * 16777619u;
    return hash ? hash : 1;
}
static int apply(const char *base_path, const char *base_afc_path, const char *profile_path,
                 const char *out_path, const char *out_afc_path) {
    uint8_t *afx = NULL, *afc = NULL; uint32_t afx_bytes, afc_bytes; afx_file_header_t header; profile_t profile;
    if (read_file(base_path, &afx, &afx_bytes) || read_file(base_afc_path, &afc, &afc_bytes) ||
        afx_file_validate(afx, afx_bytes, &header) || profile_read(profile_path, afx, afx_bytes, &profile) ||
        afc_bytes < AFX_SEEK_HEADER_BYTES || afx_read32(afc) != AFX_SEEK_MAGIC ||
        afx_read32(afc + 4) != AFX_SEEK_VERSION || afx_read32(afc + 8) != header.control_id ||
        afx_read32(afc + 12) != header.bank_id_low || afx_read32(afc + 16) != header.bank_id_high ||
        afx_read32(afc + 20) != AFX_SEEK_HEADER_BYTES || afx_read32(afc + 28) != afc_bytes) goto failed;
    uint8_t *image = afx + header.image_offset;
    for (uint32_t offset = header.stream_offset, end = header.stream_offset + header.stream_size; offset < end;) {
        afx_event_t event;
        if (afx_decode_event(image + offset, end - offset, &event) ||
            ((event.opcode == AFX_OP_NOTE || event.opcode == AFX_OP_PATCH) && (event.mask & (1u << AFX_FIELD_DSP_SEND)))) goto failed;
        offset += event.bytes;
    }
    for (uint32_t setup = 0; setup < header.setup_count; ++setup)
        afx_write16(image + setup * AFX_SETUP_BYTES + AFX_FIELD_DSP_SEND * 2u, profile.send);
    uint32_t id = control_id(image, header.image_size);
    afx_write32(afx + 32, id); afx_write32(afc + 8, id);
    if (afx_file_validate(afx, afx_bytes, NULL) || write_file(out_path, afx, afx_bytes) || write_file(out_afc_path, afc, afc_bytes)) goto failed;
    free(afx); free(afc); return 0;
failed:
    free(afx); free(afc); return -1;
}
int main(int argc, char **argv) {
    if (argc == 6 && !strcmp(argv[1], "init")) {
        uint8_t *afx; uint32_t bytes;
        if (!read_file(argv[2], &afx, &bytes) && !afx_file_validate(afx, bytes, NULL) &&
            !profile_write(argv[3], afx, bytes, argv[4], (unsigned)strtoul(argv[5], NULL, 10))) { free(afx); return 0; }
        free(afx); return fprintf(stderr, "cannot create profile\n"), 1;
    }
    if (argc == 4 && !strcmp(argv[1], "describe")) {
        uint8_t *afx; uint32_t bytes; profile_t profile;
        if (!read_file(argv[2], &afx, &bytes) && !afx_file_validate(afx, bytes, NULL) &&
            !profile_read(argv[3], afx, bytes, &profile)) { printf("%s %u\n", profile.preset, profile.send); free(afx); return 0; }
        free(afx); return fprintf(stderr, "invalid profile\n"), 1;
    }
    if (argc == 7 && !strcmp(argv[1], "apply"))
        return apply(argv[2], argv[3], argv[4], argv[5], argv[6]) ? fprintf(stderr, "cannot apply profile\n"), 1 : 0;
    return fprintf(stderr, "usage: %s init base.afx output.afp preset send | describe base.afx profile.afp | apply base.afx base.afc profile.afp output.afx output.afc\n", argv[0]), 2;
}
