#ifndef AFX_MAP_C_H
#define AFX_MAP_C_H
/* Song structure in an AFBM bank map: `section` and `order` lines (docs/specs/authoring-formats.md),
 * shared by afx_bank (MIDI songs) and afx_vgm (VGM recordings). */
#include "afx_compile_c.h"

#define AFX_MAP_NAME_BYTES 64u
#define AFX_MAP_MAX_ORDER 256u
typedef struct {
    char names[AFX_C_MAX_SECTIONS][AFX_MAP_NAME_BYTES];
    afx_c_window_t windows[AFX_C_MAX_SECTIONS];
    uint32_t section_count;
    uint16_t order[AFX_MAP_MAX_ORDER];
    uint32_t order_count, loop_index; /* loop_index: AFX_C_SONG_ENDS without loop= */
} afx_map_structure_t;

/* `section <song> <name> <start> <end> [join=new|continue] [overlap=<tick>]`: adds a section to s
 * (the caller found the song by name). Returns 0, or -1 for a bad or duplicate line. */
int afx_map_parse_section(const char *line, afx_map_structure_t *s);
/* `order <song> <section>... [loop=<section>]`: the play order, once per song, after its sections. */
int afx_map_parse_order(const char *line, afx_map_structure_t *s);
/* The song name a `section` or `order` line names (out: AFX_MAP_NAME_BYTES or more). */
int afx_map_line_song(const char *line, char *out, uint32_t out_bytes);
/* Whether the structure is complete: sections and an order. A song with sections but no order, or the
 * reverse, is an error (-1); 0: no structure (a plain flow); 1: a song. */
int afx_map_structure_state(const afx_map_structure_t *s);
#endif
