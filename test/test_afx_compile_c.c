#include "afx_compile_c.h"
#include "afx_midi_c.h"
#include <aicaflow/codec.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const afx_c_note_t notes[] = {{0, 500, 69, 120}, {250, 750, 76, 100}};
    afx_c_output_t out;
    assert(!afx_c_compile_sine(notes, 2, 1000, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    afx_c_output_t repeat;
    assert(!afx_c_compile_sine(notes, 2, 1000, &repeat));
    assert(out.afb_bytes == repeat.afb_bytes && !memcmp(out.afb, repeat.afb, out.afb_bytes));
    assert(out.afx_bytes == repeat.afx_bytes && !memcmp(out.afx, repeat.afx, out.afx_bytes));
    afx_c_output_free(&repeat);
    afx_c_output_free(&out);
    const uint8_t pcm[] = {0, 0, 0xff, 0x7f, 0, 0, 0, 0x80};
    const afx_c_pcm16_t one_shot = {pcm, 4, 60, 0, 0, 3};
    assert(!afx_c_compile_pcm16(notes, 2, 1000, &one_shot, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    assert(out.afb_bytes == 32 + sizeof(pcm) && !memcmp(out.afb + 32, pcm, sizeof(pcm)));
    afx_c_output_free(&out);
    const afx_c_note_t split_notes[] = {{0, 100, 60, 100}, {100, 200, 72, 100}};
    const afx_c_zone_t zones[] = {
        {{pcm, 4, 60, 0, 0, 3}, 0, 65},
        {{pcm, 4, 72, 1, 0, 3}, 66, 127},
    };
    assert(!afx_c_compile_zones(split_notes, 2, 1000, zones, 2, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    assert(afx_read32(out.afx + 36) == 2 && afx_read32(out.afx + 52) == 2);
    assert(afx_read32(out.afx + 80 + 4) == 0 && afx_read32(out.afx + 92 + 4) == 32);
    assert(out.afb_bytes == 72);
    afx_c_output_free(&out);
    const unsigned char midi[] = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 1,224,
        'M','T','r','k', 0,0,0,20,
        0,0xff,0x51,3,9,39,192, 0,0x90,69,100,
        0x83,0x60,0x80,69,0, 0,0xff,0x2f,0
    };
    afx_c_note_t *parsed = NULL;
    unsigned count = 0;
    assert(!afx_c_midi_notes(midi, sizeof(midi), 1000, &parsed, &count));
    assert(count == 1 && parsed[0].start_tick == 0 && parsed[0].end_tick == 600);
    assert(!afx_c_compile_sine(parsed, count, 1000, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    free(parsed); afx_c_output_free(&out);
    return 0;
}
