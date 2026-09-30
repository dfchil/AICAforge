#include "afx_compile_c.h"
#include "afx_midi_c.h"
#include "afx_sample_c.h"
#include <aicaflow/codec.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const afx_c_note_t notes[] = {{0, 500, 69, 120, 0, 0, 0, 0},
                                  {250, 750, 76, 100, 0, 0, 0, 0}};
    afx_c_output_t out;
    assert(!afx_c_compile_sine(notes, 2, 1000, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    assert(afx_read16(out.afx + afx_read32(out.afx + 16) + 18) == 0x0f10);
    assert(out.afc_bytes == 64 && afx_read32(out.afc) == AFX_SEEK_MAGIC);
    assert(out.afv_bytes > 12 && !memcmp(out.afv, "VIZ1", 4));
    /* Both active pitches cover the visual range; a later, quieter note must
       still light a different band instead of collapsing a chord to one bar. */
    assert(out.afv[12] > 0 && out.afv[12 + 15 * 32] > 0 && out.afv[12 + 15 * 32 + 31] > 0);
    afx_c_output_t repeat;
    assert(!afx_c_compile_sine(notes, 2, 1000, &repeat));
    assert(out.afb_bytes == repeat.afb_bytes && !memcmp(out.afb, repeat.afb, out.afb_bytes));
    assert(out.afx_bytes == repeat.afx_bytes && !memcmp(out.afx, repeat.afx, out.afx_bytes));
    assert(out.afc_bytes == repeat.afc_bytes && !memcmp(out.afc, repeat.afc, out.afc_bytes));
    assert(out.afv_bytes == repeat.afv_bytes && !memcmp(out.afv, repeat.afv, out.afv_bytes));
    afx_c_output_free(&repeat);
    afx_c_output_free(&out);
    const uint8_t pcm[] = {0, 0, 0xff, 0x7f, 0, 0, 0, 0x80};
    const afx_c_sample_t one_shot = {pcm, sizeof(pcm), 4, AFX_PCM16, 60, 0, 0, 3, 0, 44100};
    assert(!afx_c_compile_sample(notes, 2, 1000, &one_shot, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    assert(out.afb_bytes == 32 + sizeof(pcm) && !memcmp(out.afb + 32, pcm, sizeof(pcm)));
    afx_c_output_free(&out);
    uint8_t *encoded = NULL, format = 0;
    uint32_t encoded_bytes = 0;
    assert(!afx_c_encode_sample(pcm, 4, 0, AFX_PCM8, &encoded, &encoded_bytes, &format));
    assert(format == AFX_PCM8 && encoded_bytes == 4); free(encoded);
    assert(!afx_c_encode_sample(pcm, 4, 0, AFX_ADPCM, &encoded, &encoded_bytes, &format));
    assert(format == AFX_ADPCM && encoded_bytes == 2); free(encoded);
    assert(!afx_c_encode_sample(pcm, 4, 1, AFX_SAMPLE_AUTO, &encoded, &encoded_bytes, &format));
    assert(format == AFX_PCM8); free(encoded);
    const afx_c_note_t split_notes[] = {{0, 100, 60, 100, 0, 0, 0, 0},
                                        {100, 200, 72, 100, 0, 0, 0, 0}};
    const afx_c_zone_t zones[] = {
        {{pcm, sizeof(pcm), 4, AFX_PCM16, 60, 0, 0, 3, 0, 44100}, 0, 65, 0, 127, 0, 0, 0, 0},
        {{pcm, sizeof(pcm), 4, AFX_PCM16, 72, 1, 0, 3, 0, 44100}, 66, 127, 0, 127, 0, 0, 0, 0},
    };
    assert(!afx_c_compile_zones(split_notes, 2, 1000, zones, 2, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    assert(afx_read32(out.afx + 36) == 2 && afx_read32(out.afx + 52) == 2);
    assert(afx_read32(out.afx + 80 + 4) == 0 && afx_read32(out.afx + 92 + 4) == 0);
    assert(out.afb_bytes == 32 + sizeof(pcm));
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
