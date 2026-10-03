#include "afx_n64_cseq.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    uint8_t cseq[80] = {0};
    /* Track 0 at 68, 96 ticks/beat: program 0, C4 then an inline 96-tick
     * duration. This checks direct CSeq decoding without a MIDI file hop. */
    cseq[3] = 68; cseq[67] = 96;
    const uint8_t track[] = {0, 0xc0, 0, 0, 0x90, 60, 100, 96, 0, 0xff, 0x2f};
    memcpy(cseq + 68, track, sizeof(track));
    afx_c_note_t *notes = NULL; uint32_t count = 0, duration = 0;
    assert(!afx_c_n64_cseq_notes(cseq, 68 + sizeof(track), -1, 1000, &notes, &count, &duration));
    assert(count == 1 && duration == 500);
    assert(notes[0].start_tick == 0 && notes[0].end_tick == 500 && notes[0].key == 60 &&
           notes[0].velocity == 100 && notes[0].program == 0 && notes[0].source_track == 0);
    free(notes);
    return 0;
}
