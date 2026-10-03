#include "afx_n64_cseq.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    uint8_t cseq[100] = {0};
    /* Track 0 at 68, 96 ticks/beat: program 0, C4 then an inline 96-tick
     * duration, followed by the DKR controls that affect live voices. This
     * checks direct CSeq decoding without a MIDI file hop. */
    cseq[3] = 68; cseq[67] = 96;
    const uint8_t track[] = {0, 0xc0, 0, 0, 0x90, 60, 100, 96,
                             48, 0xb0, 7, 64, 0, 0xb0, 10, 32,
                             0, 0xb0, 91, 80, 0, 0xe0, 0, 0, 48, 0xff, 0x2f};
    memcpy(cseq + 68, track, sizeof(track));
    afx_c_note_t *notes = NULL; afx_n64_automation_t *automation = NULL;
    uint32_t count = 0, automation_count = 0, duration = 0;
    assert(!afx_c_n64_cseq_notes(cseq, 68 + sizeof(track), -1, 1000, &notes, &count,
                                 &automation, &automation_count, &duration));
    assert(count == 1 && duration == 500);
    assert(notes[0].start_tick == 0 && notes[0].end_tick == 500 && notes[0].key == 60 &&
           notes[0].velocity == 100 && notes[0].program == 0 && notes[0].source_track == 0);
    assert(automation_count == 5 && automation[0].kind == AFX_N64_EVENT_PROGRAM &&
           automation[0].channel == 0 && automation[0].value == 0);
    assert(automation[1].tick == 250 && automation[1].kind == AFX_N64_EVENT_CONTROL &&
           automation[1].control == 7 && automation[1].value == 64);
    assert(automation[2].control == 10 && automation[3].control == 91 &&
           automation[4].kind == AFX_N64_EVENT_PITCH && automation[4].pitch_bend == -8192);
    free(notes); free(automation);
    return 0;
}
