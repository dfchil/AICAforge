#include "afx_map_c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int split_words(const char *line, char *copy, uint32_t copy_bytes, char **words, uint32_t max, uint32_t *count) {
    size_t length = strlen(line);
    if (length >= copy_bytes) return -1;
    memcpy(copy, line, length + 1u); *count = 0;
    for (char *token = strtok(copy, " \t\r\n"); token; token = strtok(NULL, " \t\r\n")) {
        if (*token == '#') break;
        if (*count == max) return -1;
        words[(*count)++] = token;
    }
    return 0;
}

static int parse_tick(const char *text, uint32_t *out) {
    char *end = NULL; unsigned long value = strtoul(text, &end, 10);
    if (!*text || *end || value > 0x7fffffffUL) return -1;
    *out = (uint32_t)value; return 0;
}

static int find_section(const afx_map_structure_t *s, const char *name) {
    for (uint32_t i = 0; i < s->section_count; ++i) if (!strcmp(s->names[i], name)) return (int)i;
    return -1;
}

int afx_map_line_song(const char *line, char *out, uint32_t out_bytes) {
    char copy[4096], *words[AFX_MAP_MAX_ORDER + 4]; uint32_t count = 0;
    if (split_words(line, copy, sizeof(copy), words, AFX_MAP_MAX_ORDER + 4, &count) || count < 2 ||
        strlen(words[1]) >= out_bytes) return -1;
    strcpy(out, words[1]); return 0;
}

int afx_map_parse_section(const char *line, afx_map_structure_t *s) {
    char copy[2048], *words[16]; uint32_t count = 0;
    afx_c_window_t window = {0, 0, 0, AFX_JOIN_NEW};
    if (split_words(line, copy, sizeof(copy), words, 16, &count) || count < 5 || strcmp(words[0], "section") ||
        s->order_count || s->section_count == AFX_C_MAX_SECTIONS || strlen(words[2]) >= AFX_MAP_NAME_BYTES ||
        find_section(s, words[2]) >= 0 || parse_tick(words[3], &window.start) || parse_tick(words[4], &window.end) ||
        window.end <= window.start) return -1;
    for (uint32_t i = 5; i < count; ++i) {
        if (!strcmp(words[i], "join=new")) window.join = AFX_JOIN_NEW;
        else if (!strcmp(words[i], "join=continue")) window.join = AFX_JOIN_CONTINUE;
        else if (!strncmp(words[i], "overlap=", 8)) { if (parse_tick(words[i] + 8, &window.overlap) || window.overlap <= window.end) return -1; }
        else return -1;
    }
    if (window.join == AFX_JOIN_CONTINUE && window.overlap) return -1;
    for (uint32_t i = 0; i < s->section_count; ++i)
        if (window.start < s->windows[i].end && s->windows[i].start < window.end) return -1;
    strcpy(s->names[s->section_count], words[2]);
    s->windows[s->section_count++] = window;
    return 0;
}

int afx_map_parse_order(const char *line, afx_map_structure_t *s) {
    char copy[4096], *words[AFX_MAP_MAX_ORDER + 4]; uint32_t count = 0;
    if (split_words(line, copy, sizeof(copy), words, AFX_MAP_MAX_ORDER + 4, &count) || count < 3 ||
        strcmp(words[0], "order") || s->order_count || !s->section_count) return -1;
    s->loop_index = AFX_C_SONG_ENDS;
    int loop = -1;
    for (uint32_t i = 2; i < count; ++i) {
        if (!strncmp(words[i], "loop=", 5)) {
            if (loop >= 0 || i + 1 != count || (loop = find_section(s, words[i] + 5)) < 0) return -1;
            continue;
        }
        int section = find_section(s, words[i]);
        if (section < 0 || s->order_count == AFX_MAP_MAX_ORDER) return -1;
        s->order[s->order_count++] = (uint16_t)section;
    }
    if (!s->order_count) return -1;
    if (loop >= 0) {
        uint32_t at = 0;
        while (at < s->order_count && s->order[at] != loop) ++at;
        if (at == s->order_count) return -1;
        s->loop_index = at;
    }
    return 0;
}

int afx_map_structure_state(const afx_map_structure_t *s) {
    if (!s->section_count && !s->order_count) return 0;
    if (!s->section_count || !s->order_count) return -1;
    for (uint32_t i = 0; i < s->section_count; ++i) { /* every section plays */
        uint32_t k = 0; while (k < s->order_count && s->order[k] != i) ++k;
        if (k == s->order_count) return -1;
    }
    return 1;
}
