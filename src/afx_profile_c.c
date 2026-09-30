#include <aicaflow/codec.h>

#include <ctype.h>
#include <stdbool.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* AFP is an offline transform. The parser accepts only the small documented
 * JSON schema; the Dreamcast never sees this data. */
enum { PROFILE_VERSION = 2, PROFILE_NAME_BYTES = 48, PROFILE_TEMPLATES = 64 };
typedef struct { uint32_t mask; uint16_t values[AFX_FIELD_COUNT]; } params_t;
typedef struct { char name[PROFILE_NAME_BYTES]; params_t parameters; } template_t;
typedef struct {
    uint32_t tick, ordinal; uint8_t channel; bool has_template, used;
    char template_name[PROFILE_NAME_BYTES]; params_t parameters;
} override_t;
typedef struct {
    char sha256[65], preset[32]; params_t defaults;
    template_t templates[PROFILE_TEMPLATES]; uint32_t template_count;
    char (*setup_templates)[PROFILE_NAME_BYTES]; uint32_t setup_count;
    override_t *overrides; uint32_t override_count, override_capacity;
} profile_t;
typedef enum { JSON_UNDEFINED, JSON_OBJECT, JSON_ARRAY, JSON_STRING, JSON_PRIMITIVE } json_type_t;
typedef struct { json_type_t type; int start, end, parent; } json_token_t;
typedef struct { const char *json; size_t length, position; int parent, count; json_token_t *tokens; int capacity; } json_parser_t;
typedef struct { uint8_t *data; uint32_t used, capacity; } bytes_t;

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
static int json_alloc(json_parser_t *parser, json_type_t type, int start) {
    if (parser->count == parser->capacity) return -1;
    parser->tokens[parser->count] = (json_token_t){type, start, -1, parser->parent};
    return parser->count++;
}
static int json_parse_string(json_parser_t *parser) {
    size_t start = ++parser->position;
    while (parser->position < parser->length && parser->json[parser->position] != '"') {
        if (parser->json[parser->position] == '\\') return -1;
        ++parser->position;
    }
    if (parser->position == parser->length) return -1;
    int token = json_alloc(parser, JSON_STRING, (int)start);
    if (token < 0) return -1;
    parser->tokens[token].end = (int)parser->position;
    return 0;
}
static int json_parse_primitive(json_parser_t *parser) {
    size_t start = parser->position;
    while (parser->position < parser->length && !strchr(" \t\r\n,:]}", parser->json[parser->position])) ++parser->position;
    if (start == parser->position) return -1;
    int token = json_alloc(parser, JSON_PRIMITIVE, (int)start);
    if (token < 0) return -1;
    parser->tokens[token].end = (int)parser->position;
    --parser->position;
    return 0;
}
static int json_parse(const char *text, size_t bytes, json_token_t *tokens, int capacity) {
    json_parser_t parser = {text, bytes, 0, -1, 0, tokens, capacity};
    for (; parser.position < bytes; ++parser.position) {
        char c = text[parser.position];
        if (isspace((unsigned char)c)) continue;
        if (c == '{' || c == '[') {
            int token = json_alloc(&parser, c == '{' ? JSON_OBJECT : JSON_ARRAY, (int)parser.position);
            if (token < 0) return -1;
            parser.parent = token;
        } else if (c == '}' || c == ']') {
            if (parser.parent >= 0 && tokens[parser.parent].type == JSON_STRING)
                parser.parent = tokens[parser.parent].parent;
            int token = parser.parent;
            if (token < 0 || (c == '}' && tokens[token].type != JSON_OBJECT) || (c == ']' && tokens[token].type != JSON_ARRAY)) return -1;
            tokens[token].end = (int)parser.position + 1; parser.parent = tokens[token].parent;
        } else if (c == '"') {
            if (json_parse_string(&parser)) return -1;
        } else if (c == ':') {
            if (!parser.count || tokens[parser.count - 1].type != JSON_STRING) return -1;
            parser.parent = parser.count - 1;
        } else if (c == ',') {
            if (parser.parent >= 0 && tokens[parser.parent].type != JSON_OBJECT && tokens[parser.parent].type != JSON_ARRAY)
                parser.parent = tokens[parser.parent].parent;
        } else if (json_parse_primitive(&parser)) return -1;
    }
    return parser.parent < 0 ? parser.count : -1;
}
static int token_next(const json_token_t *tokens, int count, int token) {
    int end = tokens[token].end;
    for (++token; token < count && tokens[token].start < end; ++token) {}
    return token;
}
static int token_equals(const char *json, const json_token_t *token, const char *value) {
    size_t bytes = strlen(value);
    return token->type == JSON_STRING && (size_t)(token->end - token->start) == bytes && !memcmp(json + token->start, value, bytes);
}
static int object_value(const char *json, const json_token_t *tokens, int count, int object, const char *key) {
    if (object < 0 || object >= count || tokens[object].type != JSON_OBJECT) return -1;
    for (int item = object + 1; item < count && tokens[item].start < tokens[object].end;) {
        int value = item + 1;
        if (tokens[item].parent == object && value < count && token_equals(json, &tokens[item], key)) return value;
        item = token_next(tokens, count, value < count ? value : item);
    }
    return -1;
}
static int token_string(const char *json, const json_token_t *tokens, int token, char *out, size_t capacity) {
    if (token < 0 || tokens[token].type != JSON_STRING || (size_t)(tokens[token].end - tokens[token].start) >= capacity) return -1;
    memcpy(out, json + tokens[token].start, (size_t)(tokens[token].end - tokens[token].start));
    out[tokens[token].end - tokens[token].start] = 0; return 0;
}
static int token_unsigned(const char *json, const json_token_t *tokens, int token, unsigned *out) {
    char buffer[32], *end; unsigned long value; size_t bytes;
    if (token < 0 || tokens[token].type != JSON_PRIMITIVE || (bytes = (size_t)(tokens[token].end - tokens[token].start)) >= sizeof(buffer)) return -1;
    memcpy(buffer, json + tokens[token].start, bytes); buffer[bytes] = 0;
    if (!isdigit((unsigned char)buffer[0])) return -1;
    value = strtoul(buffer, &end, 10);
    if (*end || value > UINT_MAX) return -1;
    *out = (unsigned)value; return 0;
}
static int valid_preset(const char *preset) {
    return !strcmp(preset, "dry") || !strcmp(preset, "room") || !strcmp(preset, "room_warm") ||
           !strcmp(preset, "room_large");
}
static int field_id(const char *name) {
    static const char *const names[] = {"env_ad","env_dr","lfo","dsp_send","direct","mix","filter_level0","filter_level1","filter_level2","filter_level3","filter_level4","filter_ad","filter_dr"};
    static const int ids[] = {AFX_FIELD_ENV_AD,AFX_FIELD_ENV_DR,AFX_FIELD_LFO,AFX_FIELD_DSP_SEND,AFX_FIELD_DIRECT,AFX_FIELD_MIX,AFX_FIELD_FILTER_LEVEL0,AFX_FIELD_FILTER_LEVEL1,AFX_FIELD_FILTER_LEVEL2,AFX_FIELD_FILTER_LEVEL3,AFX_FIELD_FILTER_LEVEL4,AFX_FIELD_FILTER_AD,AFX_FIELD_FILTER_DR};
    for (uint32_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) if (!strcmp(name, names[i])) return ids[i];
    return -1;
}
static void params_merge(params_t *into, const params_t *from) {
    for (uint32_t field = 0; field < AFX_FIELD_COUNT; ++field) if (from->mask & (1u << field)) into->values[field] = from->values[field];
    into->mask |= from->mask;
}
static int parse_params(const char *json, const json_token_t *tokens, int count, int object, params_t *out) {
    if (object < 0 || tokens[object].type != JSON_OBJECT) return -1;
    for (int item = object + 1; item < count && tokens[item].start < tokens[object].end;) {
        int value = item + 1, field; char name[PROFILE_NAME_BYTES]; unsigned number;
        if (tokens[item].parent != object) { item = token_next(tokens, count, value < count ? value : item); continue; }
        if (value >= count || token_string(json, tokens, item, name, sizeof(name)) || (field = field_id(name)) < 0 ||
            (out->mask & (1u << field)) || token_unsigned(json, tokens, value, &number) || number > UINT16_MAX) return -1;
        out->mask |= 1u << field; out->values[field] = (uint16_t)number; item = token_next(tokens, count, value);
    }
    return 0;
}
static template_t *find_template(profile_t *profile, const char *name) {
    for (uint32_t i = 0; i < profile->template_count; ++i) if (!strcmp(profile->templates[i].name, name)) return &profile->templates[i];
    return NULL;
}
static int parse_templates(const char *json, const json_token_t *tokens, int count, int object, profile_t *profile) {
    if (object < 0 || tokens[object].type != JSON_OBJECT) return -1;
    for (int item = object + 1; item < count && tokens[item].start < tokens[object].end;) {
        int value = item + 1, parameters; template_t *template;
        if (tokens[item].parent != object) { item = token_next(tokens, count, value < count ? value : item); continue; }
        if (value >= count || profile->template_count == PROFILE_TEMPLATES) return -1;
        template = &profile->templates[profile->template_count++];
        if (token_string(json, tokens, item, template->name, sizeof(template->name)) || !template->name[0] || find_template(profile, template->name) != template ||
            (parameters = object_value(json, tokens, count, value, "parameters")) < 0 || parse_params(json, tokens, count, parameters, &template->parameters)) return -1;
        item = token_next(tokens, count, value);
    }
    return 0;
}
static int parse_setup_templates(const char *json, const json_token_t *tokens, int count, int object, profile_t *profile) {
    if (object < 0 || tokens[object].type != JSON_OBJECT) return -1;
    for (int item = object + 1; item < count && tokens[item].start < tokens[object].end;) {
        int value = item + 1; char key[PROFILE_NAME_BYTES], *end; unsigned long setup;
        if (tokens[item].parent != object) { item = token_next(tokens, count, value < count ? value : item); continue; }
        if (value >= count || token_string(json, tokens, item, key, sizeof(key)) || !key[0] || (setup = strtoul(key, &end, 10)) >= profile->setup_count || *end ||
            profile->setup_templates[setup][0] || token_string(json, tokens, value, profile->setup_templates[setup], PROFILE_NAME_BYTES)) return -1;
        item = token_next(tokens, count, value);
    }
    return 0;
}
static int override_push(profile_t *profile, override_t **out) {
    if (profile->override_count == profile->override_capacity) {
        uint32_t capacity = profile->override_capacity ? profile->override_capacity * 2u : 32u;
        override_t *grown = realloc(profile->overrides, (size_t)capacity * sizeof(*grown));
        if (!grown) return -1;
        profile->overrides = grown; profile->override_capacity = capacity;
    }
    *out = &profile->overrides[profile->override_count++]; **out = (override_t){0}; return 0;
}
static int parse_overrides(const char *json, const json_token_t *tokens, int count, int array, profile_t *profile) {
    if (array < 0 || tokens[array].type != JSON_ARRAY) return -1;
    for (int item = array + 1; item < count && tokens[item].start < tokens[array].end; item = token_next(tokens, count, item)) {
        override_t *override; int event, value; char kind[16]; unsigned channel;
        if (tokens[item].parent != array || override_push(profile, &override) || (event = object_value(json, tokens, count, item, "event")) < 0 ||
            (value = object_value(json, tokens, count, event, "kind")) < 0 || token_string(json, tokens, value, kind, sizeof(kind)) || strcmp(kind, "note") ||
            (value = object_value(json, tokens, count, event, "tick")) < 0 || token_unsigned(json, tokens, value, &override->tick) ||
            (value = object_value(json, tokens, count, event, "ordinal")) < 0 || token_unsigned(json, tokens, value, &override->ordinal) ||
            (value = object_value(json, tokens, count, event, "channel")) < 0 || token_unsigned(json, tokens, value, &channel) || channel >= AFX_MAX_FLOW_CHANNELS) return -1;
        override->channel = (uint8_t)channel;
        if ((value = object_value(json, tokens, count, item, "template")) >= 0) {
            if (token_string(json, tokens, value, override->template_name, sizeof(override->template_name))) return -1;
            override->has_template = true;
        }
        if ((value = object_value(json, tokens, count, item, "parameters")) >= 0 && parse_params(json, tokens, count, value, &override->parameters)) return -1;
        if (!override->has_template && !override->parameters.mask) return -1;
    }
    return 0;
}
static void profile_free(profile_t *profile) { free(profile->setup_templates); free(profile->overrides); *profile = (profile_t){0}; }
static int profile_read(const char *path, const uint8_t *base, uint32_t base_bytes, uint32_t setup_count, profile_t *out) {
    uint8_t *json = NULL; uint32_t bytes; json_token_t *tokens = NULL; int count, root, value; char format[32], sha[65]; unsigned version, afx_version;
    *out = (profile_t){0}; out->setup_count = setup_count; out->setup_templates = calloc(setup_count ? setup_count : 1u, PROFILE_NAME_BYTES);
    if (!out->setup_templates || read_file(path, &json, &bytes) || bytes > (UINT32_MAX - 64u) / 2u) goto failed;
    tokens = calloc((size_t)bytes * 2u + 64u, sizeof(*tokens));
    if (!tokens) goto failed;
    count = json_parse((char *)json, bytes, tokens, (int)((size_t)bytes * 2u + 64u));
    if (count < 1 || tokens[0].type != JSON_OBJECT) goto failed;
    root = 0;
    if ((value = object_value((char *)json, tokens, count, root, "format")) < 0 || token_string((char *)json, tokens, value, format, sizeof(format)) || strcmp(format, "aicaflow.afp") ||
        (value = object_value((char *)json, tokens, count, root, "version")) < 0 || token_unsigned((char *)json, tokens, value, &version) || version != PROFILE_VERSION ||
        (value = object_value((char *)json, tokens, count, root, "base")) < 0 || (value = object_value((char *)json, tokens, count, value, "afx_version")) < 0 || token_unsigned((char *)json, tokens, value, &afx_version) || afx_version != AFX_FILE_VERSION ||
        (value = object_value((char *)json, tokens, count, root, "base")) < 0 || (value = object_value((char *)json, tokens, count, value, "canonical_sha256")) < 0 || token_string((char *)json, tokens, value, sha, sizeof(sha)) || strlen(sha) != 64 ||
        (value = object_value((char *)json, tokens, count, root, "dsp")) < 0 || (value = object_value((char *)json, tokens, count, value, "preset")) < 0 || token_string((char *)json, tokens, value, out->preset, sizeof(out->preset)) || !valid_preset(out->preset) ||
        (value = object_value((char *)json, tokens, count, root, "defaults")) < 0 || parse_params((char *)json, tokens, count, value, &out->defaults) ||
        (value = object_value((char *)json, tokens, count, root, "templates")) < 0 || parse_templates((char *)json, tokens, count, value, out) ||
        (value = object_value((char *)json, tokens, count, root, "setup_templates")) < 0 || parse_setup_templates((char *)json, tokens, count, value, out) ||
        (value = object_value((char *)json, tokens, count, root, "overrides")) < 0 || parse_overrides((char *)json, tokens, count, value, out)) goto failed;
    digest(base, base_bytes, out->sha256);
    if (strcmp(sha, out->sha256)) goto failed;
    for (uint32_t i = 0; i < setup_count; ++i) if (out->setup_templates[i][0] && !find_template(out, out->setup_templates[i])) goto failed;
    for (uint32_t i = 0; i < out->override_count; ++i) if (out->overrides[i].has_template && !find_template(out, out->overrides[i].template_name)) goto failed;
    free(tokens); free(json); return 0;
failed:
    free(tokens); free(json); profile_free(out); return -1;
}
static int profile_write(const char *path, const uint8_t *base, uint32_t bytes, const char *preset, unsigned send) {
    char sha[65], defaults[32]; afx_file_header_t header;
    if (!valid_preset(preset) || send > 255 || ((!strcmp(preset, "dry")) != (send == 0))) return -1;
    if (afx_file_validate(base, bytes, &header)) return -1;
    digest(base, bytes, sha);
    FILE *file = fopen(path, "w");
    if (!file) return -1;
    if (send) snprintf(defaults, sizeof(defaults), "{ \"dsp_send\": %u }", send);
    else strcpy(defaults, "{}");
    int failed = fprintf(file,
        "{\n  \"format\": \"aicaflow.afp\",\n  \"version\": %u,\n"
        "  \"base\": { \"afx_version\": %u, \"canonical_sha256\": \"%s\" },\n"
        "  \"dsp\": { \"preset\": \"%s\" },\n  \"defaults\": %s,\n"
        "  \"templates\": {},\n  \"setup_templates\": {},\n  \"overrides\": []\n}\n",
        PROFILE_VERSION, AFX_FILE_VERSION, sha, preset, defaults) < 0;
    failed |= fclose(file); return failed ? -1 : 0;
}
static int bytes_reserve(bytes_t *out, uint32_t bytes) {
    if (bytes <= out->capacity) return 0;
    uint32_t capacity = out->capacity ? out->capacity : 256;
    while (capacity < bytes) { if (capacity > UINT32_MAX / 2u) return -1; capacity *= 2; }
    uint8_t *grown = realloc(out->data, capacity); if (!grown) return -1;
    out->data = grown; out->capacity = capacity; return 0;
}
static int bytes_event(bytes_t *out, const afx_event_t *event, const uint16_t values[AFX_FIELD_COUNT]) {
    uint8_t encoded[8 + AFX_SETUP_BYTES]; uint32_t bytes;
    if (afx_encode_event(encoded, sizeof(encoded), event, values, &bytes) || bytes > UINT32_MAX - out->used || bytes_reserve(out, out->used + bytes)) return -1;
    memcpy(out->data + out->used, encoded, bytes); out->used += bytes; return 0;
}
static void event_values(const afx_event_t *event, uint16_t values[AFX_FIELD_COUNT]) {
    const uint8_t *at = event->values; memset(values, 0, AFX_SETUP_BYTES);
    for (uint32_t field = 0; field < AFX_FIELD_COUNT; ++field) if (event->mask & (1u << field)) { values[field] = afx_read16(at); at += 2; }
}
static void resolved_params(profile_t *profile, uint32_t setup, uint32_t tick, uint32_t ordinal, uint8_t channel, params_t *out) {
    *out = profile->defaults;
    if (profile->setup_templates[setup][0]) params_merge(out, &find_template(profile, profile->setup_templates[setup])->parameters);
    for (uint32_t i = 0; i < profile->override_count; ++i) {
        override_t *override = &profile->overrides[i];
        if (override->tick == tick && override->ordinal == ordinal && override->channel == channel) {
            if (override->has_template) params_merge(out, &find_template(profile, override->template_name)->parameters);
            params_merge(out, &override->parameters); override->used = true; return;
        }
    }
}
static void event_params(afx_event_t *event, uint16_t values[AFX_FIELD_COUNT], const params_t *parameters) {
    for (uint32_t field = 0; field < AFX_FIELD_COUNT; ++field) if (parameters->mask & (1u << field)) values[field] = parameters->values[field];
    event->mask |= parameters->mask;
}
static int rewrite_stream(const uint8_t *image, const afx_file_header_t *header, profile_t *profile, bytes_t *stream, uint32_t *max_commands, uint32_t *max_writes) {
    uint32_t at = header->stream_offset, end = at + header->stream_size, tick = 0, ordinal = 0, ordinal_tick = UINT32_MAX, commands = 0, writes = 0;
    params_t active[AFX_MAX_FLOW_CHANNELS] = {{0}}; bool active_note[AFX_MAX_FLOW_CHANNELS] = {0}; *max_commands = *max_writes = 0;
    while (at < end) {
        afx_event_t event; uint16_t values[AFX_FIELD_COUNT];
        if (afx_decode_event(image + at, end - at, &event)) return -1;
        at += event.bytes; event_values(&event, values);
        if (event.opcode >= AFX_OP_WAIT8 && event.opcode <= AFX_OP_WAIT32) { if (commands > *max_commands) *max_commands = commands; if (writes > *max_writes) *max_writes = writes; commands = writes = 0; tick += event.wait; }
        else if (event.opcode == AFX_OP_NOTE) {
            if (event.setup >= header->setup_count) return -1;
            if (ordinal_tick != tick) { ordinal_tick = tick; ordinal = 0; }
            resolved_params(profile, event.setup, tick, ordinal++, event.channel, &active[event.channel]);
            event_params(&event, values, &active[event.channel]); active_note[event.channel] = true; ++commands; writes += 19;
        } else if (event.opcode == AFX_OP_PATCH) { if (active_note[event.channel]) event_params(&event, values, &active[event.channel]); ++commands; writes += afx_field_value_bytes(event.mask) / 2u; }
        else if (event.opcode == AFX_OP_KEYOFF) { ++commands; ++writes; active_note[event.channel] = false; }
        if (commands > AFX_EXECUTION_BUDGET_COMMANDS || writes > AFX_EXECUTION_BUDGET_WRITES || bytes_event(stream, &event, values)) return -1;
    }
    if (commands > *max_commands) *max_commands = commands; if (writes > *max_writes) *max_writes = writes;
    for (uint32_t i = 0; i < profile->override_count; ++i) if (!profile->overrides[i].used) return -1;
    return 0;
}
static int valid_seek(const uint8_t *afc, uint32_t bytes, const afx_file_header_t *header) {
    return bytes >= AFX_SEEK_HEADER_BYTES && afx_read32(afc) == AFX_SEEK_MAGIC && afx_read32(afc + 4) == AFX_SEEK_VERSION && afx_read32(afc + 8) == header->control_id && afx_read32(afc + 12) == header->bank_id_low && afx_read32(afc + 16) == header->bank_id_high && afx_read32(afc + 20) == AFX_SEEK_HEADER_BYTES && afx_read32(afc + 28) == bytes;
}
static uint8_t *build_seek(const afx_file_header_t *header, uint32_t control_id, uint32_t *bytes) {
    uint8_t *afc = calloc(1, AFX_SEEK_HEADER_BYTES + 32u); if (!afc) return NULL;
    afx_write32(afc, AFX_SEEK_MAGIC); afx_write32(afc + 4, AFX_SEEK_VERSION); afx_write32(afc + 8, control_id); afx_write32(afc + 12, header->bank_id_low); afx_write32(afc + 16, header->bank_id_high); afx_write32(afc + 20, AFX_SEEK_HEADER_BYTES); afx_write32(afc + 24, 32); afx_write32(afc + 28, AFX_SEEK_HEADER_BYTES + 32u);
    afx_write32(afc + AFX_SEEK_HEADER_BYTES, AFX_CHECKPOINT_MAGIC); afx_write32(afc + AFX_SEEK_HEADER_BYTES + 4, AFX_CHECKPOINT_VERSION); afx_write32(afc + AFX_SEEK_HEADER_BYTES + 8, 1); afx_write32(afc + AFX_SEEK_HEADER_BYTES + 20, header->stream_offset);
    *bytes = AFX_SEEK_HEADER_BYTES + 32u; return afc;
}
static int profile_effect(const profile_t *profile) {
    if (profile->defaults.mask) return 1;
    for (uint32_t i = 0; i < profile->setup_count; ++i) if (profile->setup_templates[i][0] && find_template((profile_t *)profile, profile->setup_templates[i])->parameters.mask) return 1;
    for (uint32_t i = 0; i < profile->override_count; ++i) if (profile->overrides[i].parameters.mask || (profile->overrides[i].has_template && find_template((profile_t *)profile, profile->overrides[i].template_name)->parameters.mask)) return 1;
    return 0;
}
static int apply(const char *base_path, const char *base_afc_path, const char *profile_path, const char *out_path, const char *out_afc_path) {
    uint8_t *afx = NULL, *afc = NULL, *derived = NULL, *derived_afc = NULL; uint32_t afx_bytes, afc_bytes, derived_bytes, derived_afc_bytes, commands, writes; afx_file_header_t header; profile_t profile; bytes_t stream = {0}; int result = -1;
    if (read_file(base_path, &afx, &afx_bytes) || read_file(base_afc_path, &afc, &afc_bytes) || afx_file_validate(afx, afx_bytes, &header) || !valid_seek(afc, afc_bytes, &header) || profile_read(profile_path, afx, afx_bytes, header.setup_count, &profile)) goto done;
    if (!profile_effect(&profile)) { result = write_file(out_path, afx, afx_bytes) || write_file(out_afc_path, afc, afc_bytes); goto done; }
    if (rewrite_stream(afx + header.image_offset, &header, &profile, &stream, &commands, &writes) || stream.used > UINT32_MAX - header.image_offset - header.stream_offset) goto done;
    header.stream_size = stream.used; header.image_size = header.stream_offset + stream.used; header.total_size = header.image_offset + header.image_size; header.work_profile = AFX_WORK_PROFILE(commands, writes);
    derived_bytes = header.total_size; derived = calloc(1, derived_bytes); if (!derived) goto done;
    memcpy(derived, afx, header.image_offset + header.stream_offset); memcpy(derived + header.image_offset + header.stream_offset, stream.data, stream.used);
    header.control_id = afx_control_id(derived + header.image_offset, header.image_size); afx_encode_header(derived, &header);
    if (afx_file_validate(derived, derived_bytes, NULL) || !(derived_afc = build_seek(&header, header.control_id, &derived_afc_bytes)) || write_file(out_path, derived, derived_bytes) || write_file(out_afc_path, derived_afc, derived_afc_bytes)) goto done;
    result = 0;
done:
    profile_free(&profile); free(stream.data); free(afx); free(afc); free(derived); free(derived_afc); return result;
}
static int inventory(const char *path) {
    uint8_t *afx; uint32_t bytes, tick = 0, ordinal = 0, ordinal_tick = UINT32_MAX; afx_file_header_t header; int first = 1;
    if (read_file(path, &afx, &bytes) || afx_file_validate(afx, bytes, &header)) { free(afx); return -1; }
    puts("[");
    for (uint32_t at = header.stream_offset, end = at + header.stream_size; at < end;) {
        afx_event_t event;
        if (afx_decode_event(afx + header.image_offset + at, end - at, &event)) { free(afx); return -1; }
        at += event.bytes;
        if (event.opcode >= AFX_OP_WAIT8 && event.opcode <= AFX_OP_WAIT32) tick += event.wait;
        if (event.opcode == AFX_OP_NOTE) {
            if (ordinal_tick != tick) { ordinal_tick = tick; ordinal = 0; }
            printf("%s  { \"event\": { \"kind\": \"note\", \"tick\": %u, \"ordinal\": %u, \"channel\": %u }, \"setup\": %u }\n", first ? "" : ",", tick, ordinal++, event.channel, event.setup);
            first = 0;
        }
    }
    puts("]"); free(afx); return 0;
}
int main(int argc, char **argv) {
    if (argc == 6 && !strcmp(argv[1], "init")) {
        uint8_t *afx; uint32_t bytes;
        if (!read_file(argv[2], &afx, &bytes) && !afx_file_validate(afx, bytes, NULL) &&
            !profile_write(argv[3], afx, bytes, argv[4], (unsigned)strtoul(argv[5], NULL, 10))) { free(afx); return 0; }
        free(afx); return fprintf(stderr, "cannot create profile\n"), 1;
    }
    if (argc == 4 && !strcmp(argv[1], "describe")) {
        uint8_t *afx; uint32_t bytes; afx_file_header_t header; profile_t profile;
        if (!read_file(argv[2], &afx, &bytes) && !afx_file_validate(afx, bytes, NULL) &&
            !afx_file_validate(afx, bytes, &header) && !profile_read(argv[3], afx, bytes, header.setup_count, &profile)) {
            unsigned send = profile.defaults.mask & (1u << AFX_FIELD_DSP_SEND) ? profile.defaults.values[AFX_FIELD_DSP_SEND] : 0;
            printf("%s %u\n", profile.preset, send); profile_free(&profile); free(afx); return 0;
        }
        free(afx); return fprintf(stderr, "invalid profile\n"), 1;
    }
    if (argc == 3 && !strcmp(argv[1], "inventory"))
        return inventory(argv[2]) ? fprintf(stderr, "cannot inspect AFX\n"), 1 : 0;
    if (argc == 7 && !strcmp(argv[1], "apply"))
        return apply(argv[2], argv[3], argv[4], argv[5], argv[6]) ? fprintf(stderr, "cannot apply profile\n"), 1 : 0;
    return fprintf(stderr, "usage: %s init base.afx output.afp preset send | inventory base.afx | describe base.afx profile.afp | apply base.afx base.afc profile.afp output.afx output.afc\n", argv[0]), 2;
}
