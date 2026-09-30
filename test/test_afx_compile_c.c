#include "afx_compile_c.h"
#include "afx_midi_c.h"
#include <aicaflow/codec.h>
#include <assert.h>
#include <stdlib.h>

int main(void) {
    const afx_c_note_t notes[] = {{0, 500, 69, 120}, {250, 750, 76, 100}};
    afx_c_output_t out;
    assert(!afx_c_compile_sine(notes, 2, 1000, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
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
