#include "afx_compile_c.h"
#include "afx_midi_c.h"
#include "afx_sample_c.h"
#include <aicaflow/codec.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void assert_stream_budget(const afx_c_output_t *out) {
    afx_file_header_t header;
    assert(afx_file_validate(out->afx, out->afx_bytes, &header) == AFX_OK);
    uint32_t at = header.image_offset + header.stream_offset;
    uint32_t end = at + header.stream_size, commands = 0, writes = 0;
    uint32_t peak_commands = 0, peak_writes = 0;
    while (at < end) {
        afx_event_t event;
        assert(afx_decode_event(out->afx + at, end - at, &event) == AFX_OK);
        at += event.bytes;
        if (event.opcode >= AFX_OP_WAIT8 && event.opcode <= AFX_OP_WAIT32) {
            assert(commands <= AFX_EXECUTION_BUDGET_COMMANDS);
            assert(writes <= AFX_EXECUTION_BUDGET_WRITES);
            commands = writes = 0;
        } else if (event.opcode == AFX_OP_NOTE) {
            ++commands; writes += 19;
        } else if (event.opcode == AFX_OP_KEYOFF) {
            ++commands; ++writes;
        } else if (event.opcode == AFX_OP_PATCH) {
            ++commands; writes += afx_field_value_bytes(event.mask) / 2u;
        }
        if (commands > peak_commands) peak_commands = commands;
        if (writes > peak_writes) peak_writes = writes;
    }
    assert(commands <= AFX_EXECUTION_BUDGET_COMMANDS);
    assert(writes <= AFX_EXECUTION_BUDGET_WRITES);
    assert(header.work_profile && header.work_profile == AFX_WORK_PROFILE(peak_commands, peak_writes));
}

int main(void) {
    const afx_c_note_t long_note = {.start_tick = 0, .end_tick = 5000, .key = 69, .velocity = 100};
    afx_c_output_t timed;
    assert(!afx_c_compile_sine(&long_note, 1, 2000, &timed));
    assert(afx_read32(timed.afc + 40) == 1); /* Default: ten seconds. */
    free(timed.afc); timed.afc = NULL;
    assert(!afx_c_build_seek(&timed, 1));
    assert(afx_read32(timed.afc + 40) == 3);
    for (unsigned i = 0; i < 3; ++i)
        assert(afx_read32(timed.afc + 48 + i * 56) == i * 2000);
    afx_c_output_free(&timed);
    const afx_c_note_t notes[] = {{.start_tick = 0, .end_tick = 500, .key = 69, .velocity = 120},
                                  {.start_tick = 250, .end_tick = 750, .key = 76, .velocity = 100}};
    afx_c_output_t out;
    assert(!afx_c_compile_sine(notes, 2, 1000, &out));
    assert_stream_budget(&out);
    afx_write32(out.afx + 76, 0); /* Older exports can be upgraded offline. */
    assert(!afx_c_set_work_profile(out.afx, out.afx_bytes));
    assert_stream_budget(&out);
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    assert(afx_read16(out.afx + afx_read32(out.afx + 16) + 18) == 0x0f10);
    assert(out.afc_bytes > 64 && afx_read32(out.afc) == AFX_SEEK_MAGIC);
    assert(afx_read32(out.afc + 32) == AFX_CHECKPOINT_MAGIC &&
           afx_read32(out.afc + 40) == 1 && afx_read32(out.afc + 60) == 1);
    assert(out.afv_bytes > 12 && !memcmp(out.afv, "VIZ1", 4));
    uint8_t *visual = NULL;
    uint32_t visual_bytes = 0;
    assert(!afx_c_visualize(out.afx, out.afx_bytes, &visual, &visual_bytes));
    assert(visual_bytes == out.afv_bytes && !memcmp(visual, out.afv, visual_bytes));
    free(visual);
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
    /* The pitch word compensates for an offline sample-rate choice. Without
       this, 22.05 kHz assets play an octave too high on AICA's 44.1 kHz base. */
    const afx_c_note_t root_note[] = {{.start_tick = 0, .end_tick = 100, .key = 60, .velocity = 100}};
    const afx_c_sample_t half_rate = {pcm, sizeof(pcm), 4, AFX_PCM16, 60, 0, 0, 3, 0, 22050};
    assert(!afx_c_compile_sample(root_note, 1, 1000, &half_rate, &out));
    uint32_t half_image = afx_read32(out.afx + 16), half_stream = afx_read32(out.afx + 24);
    assert(out.afx[half_image + half_stream] == AFX_OP_NOTE_PL &&
           ((afx_read16(out.afx + half_image + half_stream + 4) >> 11) & 15u) == 15u);
    afx_c_output_free(&out);
    uint8_t *encoded = NULL, format = 0;
    uint32_t encoded_bytes = 0;
    assert(!afx_c_encode_sample(pcm, 4, 0, AFX_PCM8, &encoded, &encoded_bytes, &format));
    assert(format == AFX_PCM8 && encoded_bytes == 4); free(encoded);
    assert(!afx_c_encode_sample(pcm, 4, 0, AFX_ADPCM, &encoded, &encoded_bytes, &format));
    assert(format == AFX_ADPCM && encoded_bytes == 2); free(encoded);
    assert(!afx_c_encode_sample(pcm, 4, 1, AFX_SAMPLE_AUTO, &encoded, &encoded_bytes, &format));
    assert(format == AFX_PCM8); free(encoded);
    /* Matches the retired Python/SciPy resample_poly(3, 2) reference.  The
       C authoring path must not silently fall back to the old linear sampler. */
    const uint8_t resample_source[] = {
        0,0, 0xe8,3, 0x30,0xf8, 0xb8,0xb, 0x60,0xf0, 0x88,0x13, 0x90,0xe8, 0x58,0x1b
    };
    const uint8_t resample_expected[] = {
        0,0, 0xdd,8, 0x4e,0xfa, 0x2f,0xf8, 0xa4,0xd, 0xa0,0xfe,
        0x5e,0xf0, 0x83,0xe, 0x43,0x7, 0x8c,0xe8, 0x8c,0x7, 0x6,0x1f
    };
    uint8_t *resampled = NULL;
    uint32_t resampled_frames = 0;
    assert(!afx_c_resample_pcm16(resample_source, 8, 2, 3, &resampled, &resampled_frames));
    assert(resampled_frames == 12 && !memcmp(resampled, resample_expected, sizeof(resample_expected)));
    free(resampled);
    /* This is the actual 44.1 kHz -> 27 kHz ratio used by the cello bank.
       An impulse exercises the long polyphase filter rather than just its
       short edge behaviour. */
    const uint8_t cello_rate_source[74] = {
        [24] = 0xe0, [25] = 0x2e, [26] = 0x60, [27] = 0xf0,
        [28] = 0xd0, [29] = 0x07
    };
    const uint8_t cello_rate_expected[] = {
        0xc2,0xff,0x69,0,0x57,0xff,0x08,1,0x65,0xfe,0xa2,2,0xe7,0xfa,0x01,0x16,
        0x54,5,0x35,0xff,0x70,1,0x02,0xff,0xa6,0,0x98,0xff,0x3e,0,0xdf,0xff,
        0x0f,0,0xfc,0xff,0xfd,0xff,0,0,0,0,0,0,0,0
    };
    assert(!afx_c_resample_pcm16(cello_rate_source, 37, 44100, 27000,
                                 &resampled, &resampled_frames));
    assert(resampled_frames == 23 && !memcmp(resampled, cello_rate_expected, sizeof(cello_rate_expected)));
    free(resampled);
    const afx_c_note_t split_notes[] = {{.start_tick = 0, .end_tick = 100, .key = 60, .velocity = 100},
                                        {.start_tick = 100, .end_tick = 200, .key = 72, .velocity = 100}};
    const afx_c_zone_t zones[] = {
        {.sample = {pcm, sizeof(pcm), 4, AFX_PCM16, 60, 0, 0, 3, 0, 44100},
         .key_min = 0, .key_max = 65, .velocity_max = 127},
        {.sample = {pcm, sizeof(pcm), 4, AFX_PCM16, 72, 1, 0, 3, 0, 44100},
         .key_min = 66, .key_max = 127, .velocity_max = 127},
    };
    assert(!afx_c_compile_zones(split_notes, 2, 1000, zones, 2, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    assert(afx_read32(out.afx + 36) == 2 && afx_read32(out.afx + 52) == 2);
    assert(afx_read32(out.afx + 80 + 4) == 0 && afx_read32(out.afx + 92 + 4) == 0);
    assert(out.afb_bytes == 32 + sizeof(pcm));
    afx_c_output_free(&out);
    /* Setup optimization must not reorder or drop bank samples: AFI indices
       and other songs sharing this bank use the original zone order. */
    const uint8_t bank_pcm[][8] = {{1}, {2}, {3}};
    afx_c_zone_t bank_zones[4];
    for (unsigned i = 0; i < 4; ++i) {
        bank_zones[i] = zones[0];
        bank_zones[i].sample.data = bank_pcm[i % 3];
    }
    const afx_c_note_t bank_notes[] = {
        {.start_tick = 0, .end_tick = 100, .key = 60, .velocity = 100, .setup_index = 3},
        {.start_tick = 100, .end_tick = 200, .key = 60, .velocity = 100, .setup_index = 4},
    };
    assert(!afx_c_compile_zones(bank_notes, 2, 1000, bank_zones, 4, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    assert(out.afb_bytes == 32 + 64 + sizeof(bank_pcm[0]));
    for (unsigned i = 0; i < 3; ++i)
        assert(!memcmp(out.afb + 32 + i * 32, bank_pcm[i], sizeof(bank_pcm[i])));
    assert(afx_read32(out.afx + 36) == 2);
    assert(afx_read32(out.afx + 80 + 4) == 64 && afx_read32(out.afx + 92 + 4) == 0);
    assert(!afx_c_compile_zones(bank_notes + 1, 1, 1000, bank_zones, 4, &repeat));
    assert(out.afb_bytes == repeat.afb_bytes && !memcmp(out.afb, repeat.afb, out.afb_bytes));
    afx_c_output_free(&repeat);
    afx_c_output_free(&out);
    /* A dense MIDI chord is emitted in adjacent millisecond slices rather
       than producing an AICAflow which ARM7 cannot activate. */
    afx_c_note_t dense[10];
    for (unsigned i = 0; i < 10; ++i)
        dense[i] = (afx_c_note_t){.start_tick = 0, .end_tick = 100, .key = (uint8_t)(60 + i), .velocity = 100};
    assert(!afx_c_compile_sample(dense, 10, 1000, &one_shot, &out));
    assert_stream_budget(&out);
    afx_c_output_free(&out);
    /* Trace importers use the same bank writer, but retain live register
       updates instead of reducing their source to MIDI notes. */
    const afx_c_zone_t trace_zone[] = {{
        .sample = one_shot, .key_max = 127, .velocity_max = 127
    }};
    const afx_c_event_t trace_events[] = {
        {.tick = 0, .order = 0, .opcode = AFX_OP_NOTE, .channel = 0, .setup = 0,
         .mask = AFX_NOTE_PL_MASK, .fields = {[AFX_FIELD_PITCH] = 0x0800, [AFX_FIELD_MIX] = 0x3024}},
        {.tick = 10, .order = 0, .opcode = AFX_OP_PATCH, .channel = 0,
         .mask = 1u << AFX_FIELD_MIX, .fields = {[AFX_FIELD_MIX] = 0x5024}},
        {.tick = 20, .order = 0, .opcode = AFX_OP_KEYOFF, .channel = 0},
    };
    assert(!afx_c_compile_events(trace_events, 3, 30, 1000, trace_zone, 1, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    uint32_t trace_image = afx_read32(out.afx + 16);
    assert(out.afx[trace_image + AFX_SETUP_BYTES] == AFX_OP_NOTE_PL &&
           out.afx[trace_image + AFX_SETUP_BYTES + 10] == AFX_OP_PATCH_LEVEL);
    afx_c_output_free(&out);
    /* One zone can yield several setups: each envelope used often enough
       gets a template of its own, so there are more templates than zones. */
    afx_c_event_t variant_notes[24];
    for (unsigned i = 0; i < 24; ++i)
        variant_notes[i] = (afx_c_event_t){.tick = 2 * i, .order = 0, .opcode = AFX_OP_NOTE, .channel = 0,
            .mask = AFX_NOTE_PL_MASK | 1u << AFX_FIELD_ENV_AD,
            .fields = {[AFX_FIELD_PITCH] = 0x0800, [AFX_FIELD_MIX] = 0x3024,
                       [AFX_FIELD_ENV_AD] = (uint16_t)(0x0010 + i % 3)}};
    afx_c_event_t *variant_events = NULL;
    afx_c_zone_t *variant_setups = NULL;
    uint32_t variant_setup_count = 0;
    assert(!afx_c_optimize_events(variant_notes, 24, trace_zone, 1, &variant_events, &variant_setups,
                                  &variant_setup_count));
    assert(variant_setup_count == 3);
    for (unsigned i = 0; i < 24; ++i) {
        const afx_c_event_t *note = &variant_events[i];
        assert(!(note->mask & 1u << AFX_FIELD_ENV_AD) &&
               variant_setups[note->setup].setup[AFX_FIELD_ENV_AD] == variant_notes[i].fields[AFX_FIELD_ENV_AD]);
    }
    assert(!afx_c_compile_events(variant_events, 24, 50, 1000, variant_setups, variant_setup_count, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    afx_c_output_free(&out);
    free(variant_events);
    free(variant_setups);
    /* Twenty notes on one tick are 380 register writes. The emitter keeps
       their order and moves what does not fit into the following ticks. */
    afx_c_event_t burst[40];
    for (unsigned i = 0; i < 20; ++i) {
        burst[i] = (afx_c_event_t){.tick = 0, .order = i, .opcode = AFX_OP_NOTE, .channel = (uint8_t)i,
            .mask = AFX_NOTE_PL_MASK, .fields = {[AFX_FIELD_PITCH] = 0x0800, [AFX_FIELD_MIX] = 0x3024}};
        burst[20 + i] = (afx_c_event_t){.tick = 20, .order = i, .opcode = AFX_OP_KEYOFF, .channel = (uint8_t)i};
    }
    assert(!afx_c_compile_events(burst, 40, 30, 1000, trace_zone, 1, &out));
    assert_stream_budget(&out);
    {
        afx_file_header_t header;
        assert(afx_file_validate(out.afx, out.afx_bytes, &header) == AFX_OK);
        uint32_t at = header.image_offset + header.stream_offset, end = at + header.stream_size;
        uint32_t tick = 0, notes_at[4] = {0}, last_channel = 0, keyoffs = 0;
        while (at < end) {
            afx_event_t event;
            assert(afx_decode_event(out.afx + at, end - at, &event) == AFX_OK);
            at += event.bytes;
            if (event.opcode >= AFX_OP_WAIT8 && event.opcode <= AFX_OP_WAIT32) tick += event.wait;
            else if (event.opcode == AFX_OP_NOTE) {
                assert(tick < 4 && event.channel >= last_channel);
                ++notes_at[tick]; last_channel = event.channel;
            } else if (event.opcode == AFX_OP_KEYOFF) { assert(tick == 20); ++keyoffs; }
        }
        /* Nine notes fit in 171 writes; later events keep their own ticks. */
        assert(notes_at[0] == 9 && notes_at[1] == 9 && notes_at[2] == 2 && keyoffs == 20 && tick == 30);
    }
    afx_c_output_free(&out);
    /* Songs: intro, A, A, B, looping to the first A, every join a new play.
       A's note rings 15 ticks past its 10-tick body: its tail. */
    {
        const afx_c_event_t note = {.opcode = AFX_OP_NOTE, .mask = AFX_NOTE_PL_MASK,
                                    .fields = {[AFX_FIELD_PITCH] = 0x0800, [AFX_FIELD_MIX] = 0x3024}};
        afx_c_event_t intro[2] = {note, {.tick = 4, .opcode = AFX_OP_KEYOFF}};
        afx_c_event_t a[3] = {note, {.tick = 15, .opcode = AFX_OP_KEYOFF},
                              {.tick = 15, .order = 1, .opcode = AFX_OP_FREE}};
        afx_c_event_t b[2] = {note, {.tick = 6, .opcode = AFX_OP_KEYOFF}};
        b[0].channel = 1;
        b[1].channel = 1;
        const afx_c_section_t sections[] = {
            {intro, 2, 5, 5, AFX_JOIN_NEW}, {a, 3, 10, 16, AFX_JOIN_NEW}, {b, 2, 8, 8, AFX_JOIN_NEW},
        };
        const uint16_t order[] = {0, 1, 1, 2};
        afx_song_header_t h;
        afx_validation_profile_t work;
        uint32_t channels, voices;
        assert(!afx_c_compile_song(sections, 3, order, 4, 1, 1000, trace_zone, 1, &out));
        assert(out.afm && out.afb && !out.afx && !out.afc && !out.afv);
        assert(afx_song_validate_profile(out.afm, out.afm_bytes, &h, &work) == AFX_OK);
        assert(afx_song_measure(out.afm, out.afm_bytes, NULL, &channels, &voices) == AFX_OK);
        /* A's note and the next play's at each A join: two voices; B uses channel 1. */
        assert(h.channels == 2 && channels == 2 && h.voices == voices && voices == 2);
        assert(h.work_profile == AFX_WORK_PROFILE(work.peak_commands, work.peak_register_writes));
        assert(h.bank_id_low == afx_read32(out.afb + 8) && !out.moved_events);
        /* A's stream: NOTE, WAIT 10, NEXT new, WAIT 5, KEYOFF, FREE, WAIT 1, END. */
        afx_song_image_header_t ih;
        assert(afx_song_validate_layout(out.afm, out.afm_bytes, NULL, &ih) == AFX_OK);
        const uint8_t *image = out.afm + h.image_offset;
        afx_song_section_t sa = afx_song_section(image, &ih, 1);
        assert(sa.body_ticks == 10 && ih.order_count == 4 && ih.loop_index == 1);
        static const uint8_t want_ops[] = {AFX_OP_NOTE, AFX_OP_WAIT8, AFX_OP_NEXT, AFX_OP_WAIT8,
                                           AFX_OP_KEYOFF, AFX_OP_FREE, AFX_OP_WAIT8, AFX_OP_END};
        for (uint32_t at = sa.stream_offset, i = 0; at < sa.stream_offset + sa.stream_size; ++i) {
            afx_event_t event;
            assert(afx_decode_event(image + at, sa.stream_offset + sa.stream_size - at, &event) == AFX_OK);
            assert(i < sizeof(want_ops) && event.opcode == (want_ops[i] == AFX_OP_NOTE ? AFX_OP_NOTE : want_ops[i]));
            at += event.bytes;
        }
        afx_c_output_free(&out);
        /* The join: 30 key-offs ring past A's body, into the tick a head of 9
           notes (171 writes) fills: they move later, and the move is reported. */
        afx_c_event_t crowded[40], head[9];
        for (uint32_t i = 0; i < 30; ++i)
            crowded[i] = (afx_c_event_t){.tick = 10, .order = i, .opcode = AFX_OP_KEYOFF, .channel = (uint8_t)i};
        for (uint32_t i = 0; i < 9; ++i) {
            head[i] = note;
            head[i].order = i;
            head[i].channel = (uint8_t)i;
        }
        const afx_c_section_t busy[] = {{crowded, 30, 10, 12, AFX_JOIN_NEW}, {head, 9, 20, 20, AFX_JOIN_NEW}};
        const uint16_t busy_order[] = {0, 1};
        assert(!afx_c_compile_song(busy, 2, busy_order, 2, AFX_C_SONG_ENDS, 1000, trace_zone, 1, &out));
        assert(afx_song_validate_profile(out.afm, out.afm_bytes, &h, &work) == AFX_OK);
        assert(work.peak_register_writes <= AFX_EXECUTION_BUDGET_WRITES && out.moved_events == 30 &&
               out.moved_ticks == 30);
        afx_c_output_free(&out);
        /* Refused: a continue join with a tail, a body pushed across its join, a
           tail still playing at the next new-play join, a bad order or loop. */
        const afx_c_section_t tailed_continue[] = {{a, 3, 10, 16, AFX_JOIN_CONTINUE}};
        const uint16_t once[] = {0};
        assert(afx_c_compile_song(tailed_continue, 1, once, 1, 0, 1000, trace_zone, 1, &out));
        afx_c_event_t pushed[40];
        for (uint32_t i = 0; i < 40; ++i)
            pushed[i] = (afx_c_event_t){.tick = 9, .order = i, .opcode = AFX_OP_KEYOFF, .channel = (uint8_t)i};
        const afx_c_section_t across[] = {{pushed, 40, 10, 10, AFX_JOIN_NEW}};
        assert(afx_c_compile_song(across, 1, once, 1, AFX_C_SONG_ENDS, 1000, trace_zone, 1, &out));
        const afx_c_section_t long_tail[] = {{a, 3, 10, 30, AFX_JOIN_NEW}};
        const uint16_t again[] = {0, 0};
        assert(afx_c_compile_song(long_tail, 1, again, 2, 0, 1000, trace_zone, 1, &out));
        const uint16_t past[] = {0, 3};
        assert(afx_c_compile_song(sections, 3, past, 2, 0, 1000, trace_zone, 1, &out));
        assert(afx_c_compile_song(sections, 3, order, 4, 4, 1000, trace_zone, 1, &out));
        afx_c_event_t parked[2] = {note, {.tick = 10, .opcode = AFX_OP_PARK}};
        const afx_c_section_t park[] = {{parked, 2, 10, 10, AFX_JOIN_NEW}};
        assert(afx_c_compile_song(park, 1, once, 1, AFX_C_SONG_ENDS, 1000, trace_zone, 1, &out));
        /* Lanes: one per local channel the song uses. */
        const uint8_t lanes[] = {2, 5};
        assert(!afx_c_compile_song_lanes(sections, 3, order, 4, 1, lanes, 2, 1000, trace_zone, 1, &out));
        assert(afx_song_validate(out.afm, out.afm_bytes, &h) == AFX_OK && (h.flags & AFX_FLAG_LANES));
        afx_c_output_free(&out);
        assert(afx_c_compile_song_lanes(sections, 3, order, 4, 1, lanes, 1, 1000, trace_zone, 1, &out));
    }
    /* afx_c_compile_zones_song: a MIDI timeline that repeats from a loop tick
       becomes a continue-only song, intro then body; zero makes it all body. */
    {
        const afx_c_note_t notes[] = {
            {.start_tick = 0, .end_tick = 600, .key = 60, .velocity = 100},
            {.start_tick = 500, .end_tick = 1500, .key = 64, .velocity = 100},
            {.start_tick = 1200, .end_tick = 2000, .key = 67, .velocity = 100},
        };
        const afx_c_zone_t zone = {.sample = one_shot, .key_max = 127, .velocity_max = 127};
        afx_song_header_t h;
        afx_song_image_header_t ih;
        static const uint32_t loop_ticks[] = {1000, 0};
        for (unsigned i = 0; i < 2; ++i) {
            assert(!afx_c_compile_zones_song(notes, 3, 1000, loop_ticks[i], &zone, 1, &out));
            assert(out.afm && afx_song_validate(out.afm, out.afm_bytes, &h) == AFX_OK && !h.voices);
            assert(afx_song_validate_layout(out.afm, out.afm_bytes, NULL, &ih) == AFX_OK);
            assert(ih.section_count == (i ? 1u : 2u) && ih.loop_index == (i ? 0u : 1u));
            afx_song_section_t body = afx_song_section(out.afm + h.image_offset, &ih, ih.section_count - 1u);
            assert(body.body_ticks == 2000 - loop_ticks[i]);
            afx_c_output_free(&out);
        }
        assert(afx_c_compile_zones_song(notes, 3, 1000, 2000, &zone, 1, &out));
        assert(!afx_c_compile_zones_song(notes, 3, 1000, AFX_C_NO_LOOP, &zone, 1, &out) && out.afx && !out.afm);
        afx_c_output_free(&out);
    }
    /* A release tail occupies its channel, but KEYOFF remains at the musical
       note end.  This is how the offline bank builder preserves an SF2
       release without inventing an ARM7-specific command. */
    const afx_c_note_t tailed[] = {
        {.start_tick = 0, .end_tick = 1100, .release_tick = 100, .key = 60, .velocity = 100},
        {.start_tick = 500, .end_tick = 600, .release_tick = 600, .key = 72, .velocity = 100},
    };
    assert(!afx_c_compile_sample(tailed, 2, 1000, &one_shot, &out));
    assert(afx_read32(out.afx + 64) == 2);
    afx_file_header_t tailed_header;
    assert(afx_file_validate(out.afx, out.afx_bytes, &tailed_header) == AFX_OK);
    uint32_t tailed_at = tailed_header.image_offset + tailed_header.stream_offset;
    uint32_t tailed_end = tailed_at + tailed_header.stream_size, tailed_tick = 0;
    int keyoff_at_note_end = 0;
    while (tailed_at < tailed_end) {
        afx_event_t event;
        assert(afx_decode_event(out.afx + tailed_at, tailed_end - tailed_at, &event) == AFX_OK);
        tailed_at += event.bytes;
        if (event.opcode == AFX_OP_WAIT8 || event.opcode == AFX_OP_WAIT16 || event.opcode == AFX_OP_WAIT32)
            tailed_tick += event.wait;
        else if (event.opcode == AFX_OP_KEYOFF && tailed_tick == 100) keyoff_at_note_end = 1;
        if (event.opcode == AFX_OP_END) break;
    }
    assert(keyoff_at_note_end);
    /* The control stream must remain alive through the release tail; ending
       directly after the final KEYOFF truncates it in the runtime. */
    assert(tailed_tick == 1100);
    afx_c_output_free(&out);
    /* Static source controls lower into ordinary setup words, while a source
       velocity law can supply the NOTE's final MIX word. */
    const afx_c_note_t controlled_note[] = {{.start_tick = 0, .end_tick = 100, .key = 60,
                                               .velocity = 100, .mix = 0x4324}};
    const afx_c_zone_t controlled_zone[] = {{
        .sample = {pcm, sizeof(pcm), 4, AFX_PCM16, 60, 0, 0, 3, 0, 44100},
        .key_max = 127, .velocity_max = 127, .dsp_send = 0x70,
        .setup_mask = (1u << AFX_FIELD_ENV_AD) | (1u << AFX_FIELD_DIRECT),
        .setup = {[AFX_FIELD_ENV_AD] = 0x1234, [AFX_FIELD_DIRECT] = 0x0f10}
    }};
    assert(!afx_c_compile_zones(controlled_note, 1, 1000, controlled_zone, 1, &out));
    uint32_t image = afx_read32(out.afx + 16);
    assert(afx_read16(out.afx + image + 8) == 0x1234 && afx_read16(out.afx + image + 16) == 0x70);
    assert(afx_read16(out.afx + image + 36 + 6) == 0x4324);
    afx_c_output_free(&out);
    /* MIDI/SF2 lowering selects both zones, but the common compiler keeps one
       sample template and emits only the differing DIRECT word at NOTE-on. */
    const afx_c_note_t templated_notes[] = {
        {.start_tick = 0, .end_tick = 100, .key = 60, .velocity = 100, .setup_index = 1},
        {.start_tick = 100, .end_tick = 200, .key = 60, .velocity = 100, .setup_index = 2},
    };
    const afx_c_zone_t templated_zones[] = {
        {.sample = one_shot, .key_max = 127, .velocity_max = 127,
         .setup_mask = 1u << AFX_FIELD_DIRECT, .setup = {[AFX_FIELD_DIRECT] = 0x0f10}},
        {.sample = one_shot, .key_max = 127, .velocity_max = 127,
         .setup_mask = 1u << AFX_FIELD_DIRECT, .setup = {[AFX_FIELD_DIRECT] = 0x0f11}},
    };
    assert(!afx_c_compile_zones(templated_notes, 2, 1000, templated_zones, 2, &out));
    assert(afx_read32(out.afx + 36) == 1);
    afx_file_header_t templated_header;
    assert(afx_file_validate(out.afx, out.afx_bytes, &templated_header) == AFX_OK);
    uint32_t templated_at = templated_header.image_offset + templated_header.stream_offset;
    uint32_t templated_end = templated_at + templated_header.stream_size, full_note = 0;
    while (templated_at < templated_end) {
        afx_event_t event;
        assert(afx_decode_event(out.afx + templated_at, templated_end - templated_at, &event) == AFX_OK);
        templated_at += event.bytes;
        if (event.opcode == AFX_OP_NOTE && event.mask & (1u << AFX_FIELD_DIRECT)) ++full_note;
    }
    assert(full_note == 1);
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
    /* A zero-length note (on, then its off, on one tick) used to lose its off
       to the sort and hang until the end of the file. It now sounds for one
       control tick, and so does a note shorter than a control tick. Division
       480 at 600 ms per beat: one MIDI tick is 1.25 ms. */
    const unsigned char short_midi[] = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 1,224,
        'M','T','r','k', 0,0,0,38,
        0,0xff,0x51,3,9,39,192,
        0,0x90,60,100, 0,0x80,60,0,          /* zero length at tick 0 */
        0x83,0x60,0x90,62,100, 0,0x80,62,0,  /* zero length at tick 480 */
        0x83,0x60,0x90,64,100, 0x83,0x60,0x80,64,0, /* a normal note */
        0,0xff,0x2f,0
    };
    assert(!afx_c_midi_notes(short_midi, sizeof(short_midi), 1000, &parsed, &count));
    assert(count == 3 && parsed[0].start_tick == 0 && parsed[0].end_tick == 1);
    assert(parsed[1].start_tick == 600 && parsed[1].end_tick == 601);
    assert(parsed[2].start_tick == 1200 && parsed[2].end_tick == 1800);
    free(parsed);
    /* The bend wheel covers the channel's RPN 0 range, not a fixed two
       semitones: full bend up with a range of 12 is an octave. */
    const unsigned char bend_midi[] = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 1,224,
        'M','T','r','k', 0,0,0,29,
        0,0xb0,101,0, 0,0xb0,100,0, 0,0xb0,6,12,
        0,0xe0,0x7f,0x7f, 0,0x90,69,100,
        0x83,0x60,0x80,69,0, 0,0xff,0x2f,0
    };
    const unsigned char octave_midi[] = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 1,224,
        'M','T','r','k', 0,0,0,13,
        0,0x90,81,100,
        0x83,0x60,0x80,81,0, 0,0xff,0x2f,0
    };
    {
        afx_c_output_t bent, octave;
        afx_c_note_t *plain = NULL; unsigned plain_count = 0;
        assert(!afx_c_midi_notes(bend_midi, sizeof(bend_midi), 1000, &parsed, &count));
        assert(count == 1 && parsed[0].pitch_sensitivity == 12);
        assert(!afx_c_compile_sine(parsed, count, 1000, &bent));
        assert(!afx_c_midi_notes(octave_midi, sizeof(octave_midi), 1000, &plain, &plain_count));
        assert(!afx_c_compile_sine(plain, plain_count, 1000, &octave));
        /* 8191/8192 of an octave rounds to the octave's own pitch word. */
        assert(bent.afx_bytes == octave.afx_bytes);
        afx_file_header_t bent_header;
        assert(afx_file_validate(bent.afx, bent.afx_bytes, &bent_header) == AFX_OK);
        uint32_t stream = bent_header.image_offset + bent_header.stream_offset;
        assert(!memcmp(bent.afx + stream, octave.afx + stream, bent_header.stream_size));
        free(parsed); free(plain); afx_c_output_free(&bent); afx_c_output_free(&octave);
    }
    /* Repeated NOTE-on for one MIDI key is a FIFO stack, not an implicit
       NOTE-off. Both notes must keep their own later key release. */
    const unsigned char repeated_key_midi[] = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 1,224,
        'M','T','r','k', 0,0,0,20,
        0,0x90,60,100, 0,0x90,60,90, 0x60,0x80,60,0, 0x60,0x80,60,0, 0,0xff,0x2f,0
    };
    assert(!afx_c_midi_notes(repeated_key_midi, sizeof(repeated_key_midi), 1000, &parsed, &count));
    assert(count == 2 && parsed[0].end_tick == 100 && parsed[1].end_tick == 200);
    free(parsed);
    /* Volume, expression and bend are source state at NOTE-on. The C SF2
       lowerer consumes them offline; ARM7 never interprets MIDI controllers. */
    const unsigned char controller_midi[] = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 1,224,
        'M','T','r','k', 0,0,0,28,
        0,0xb0,7,64, 0,0xb0,11,63, 0,0xb0,91,100, 0,0xe0,0,65, 0,0x90,69,100,
        0x60,0x80,69,0, 0,0xff,0x2f,0
    };
    assert(!afx_c_midi_notes(controller_midi, sizeof(controller_midi), 1000, &parsed, &count));
    assert(count == 1 && parsed[0].controller_state >> 39 &&
           ((parsed[0].controller_state >> 4) & 127u) == 64 &&
           ((parsed[0].controller_state >> 11) & 127u) == 63 &&
           ((parsed[0].controller_state >> 18) & 127u) == 64 &&
           ((parsed[0].controller_state >> 25) & 16383u) == 8320 &&
           parsed[0].controllers[91] == 100);
    free(parsed);
    /* Type-1 tracks are one MIDI channel timeline: a program event in the
       control track must apply to notes placed in another track. */
    const unsigned char split_midi[] = {
        'M','T','h','d', 0,0,0,6, 0,1, 0,2, 1,224,
        'M','T','r','k', 0,0,0,12,
        0,0x90,60,100, 0x60,0x80,60,0, 0,0xff,0x2f,0,
        'M','T','r','k', 0,0,0,7,
        0,0xc0,42, 0,0xff,0x2f,0
    };
    assert(!afx_c_midi_notes(split_midi, sizeof(split_midi), 1000, &parsed, &count));
    assert(count == 1 && parsed[0].program == 42 && parsed[0].end_tick == 100);
    free(parsed);
    /* Sustain pedal is baked into the offline note lifetime.  The ARM7 sees
       only the resulting KEYOFF at pedal release, exactly as for a native
       long note. */
    const unsigned char sustain_midi[] = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 1,224,
        'M','T','r','k', 0,0,0,20,
        0,0x90,60,100, 10,0xb0,64,127, 10,0x80,60,0,
        0x50,0xb0,64,0, 0,0xff,0x2f,0
    };
    assert(!afx_c_midi_notes(sustain_midi, sizeof(sustain_midi), 1000, &parsed, &count));
    assert(count == 1 && parsed[0].start_tick == 0 && parsed[0].end_tick == 104);
    free(parsed);
    /* Lossy trace simplification: pan and level follow the source to within
       the tolerance, measured from the last value kept. */
    enum { DIRECT = 1u << AFX_FIELD_DIRECT, MIX = 1u << AFX_FIELD_MIX, PITCH = 1u << AFX_FIELD_PITCH };
#define NOTE_EVENT(t, ch, m, dir, mx) {.tick = (t), .order = (t), .mask = (m), .opcode = AFX_OP_NOTE, .channel = (ch), \
                                       .fields = {[AFX_FIELD_DIRECT] = (dir), [AFX_FIELD_MIX] = (mx)}}
#define PATCH_EVENT(t, ch, m, dir, mx) {.tick = (t), .order = (t), .mask = (m), .opcode = AFX_OP_PATCH, .channel = (ch), \
                                        .fields = {[AFX_FIELD_DIRECT] = (dir), [AFX_FIELD_MIX] = (mx)}}
    const afx_c_simplify_t lossy = {.pan_tolerance = 2, .level_tolerance = 1, .loop_tick = AFX_C_NO_LOOP};
    afx_c_event_t trace[] = {
        NOTE_EVENT(0, 0, DIRECT | MIX, 0x0805, 0x4020),
        PATCH_EVENT(2, 0, DIRECT, 0x0807, 0),         /* Two steps from 5: dropped. */
        PATCH_EVENT(3, 0, DIRECT, 0x0808, 0),         /* Three from the 5 still held: kept. */
        PATCH_EVENT(4, 0, DIRECT, 0x0818, 0),         /* +8 to -8 crosses centre: kept. */
        PATCH_EVENT(5, 0, DIRECT, 0x0918, 0),         /* The same pan, another send level: kept. */
        PATCH_EVENT(6, 0, MIX | PITCH, 0, 0x4120),    /* One step: the MIX goes, the pitch stays. */
        PATCH_EVENT(7, 0, MIX, 0, 0x4220),            /* Two from 0x40: kept. */
        PATCH_EVENT(8, 0, MIX, 0, 0x4221),            /* The same level, another Q: kept. */
        PATCH_EVENT(9, 0, MIX, 0, 0xFE21),
        PATCH_EVENT(10, 0, MIX, 0, 0xFF21),           /* One step, but to silence: kept. */
        PATCH_EVENT(11, 1, DIRECT, 0x0805, 0),        /* Another channel: nothing known yet. */
        NOTE_EVENT(12, 1, MIX, 0, 0x4020),            /* DIRECT left to the setup: unknown. */
        PATCH_EVENT(13, 1, DIRECT, 0x0805, 0),
        PATCH_EVENT(14, 1, MIX, 0, 0x4020),           /* The NOTE gave this one: dropped. */
    };
    const uint32_t expected_masks[] = {DIRECT | MIX, DIRECT, DIRECT, DIRECT, PITCH, MIX, MIX, MIX, MIX,
                                       DIRECT, MIX, DIRECT};
    const uint32_t expected_ticks[] = {0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    count = sizeof(trace) / sizeof(*trace);
    /* Input order does not matter. */
    afx_c_event_t last = trace[count - 1];
    trace[count - 1] = trace[0]; trace[0] = last;
    assert(!afx_c_simplify_events(trace, &count, &lossy));
    assert(count == sizeof(expected_masks) / sizeof(*expected_masks));
    for (uint32_t i = 0; i < count; ++i)
        assert(trace[i].mask == expected_masks[i] && trace[i].tick == expected_ticks[i]);
    /* A loop returns to its tick with the state the end left. */
    afx_c_event_t looped[] = {
        NOTE_EVENT(0, 0, DIRECT, 0x0805, 0),
        PATCH_EVENT(1, 0, DIRECT, 0x0806, 0),
        PATCH_EVENT(10, 0, DIRECT, 0x0806, 0),        /* The first change from the loop on: kept. */
        PATCH_EVENT(11, 0, DIRECT, 0x0807, 0),
    };
    afx_c_simplify_t loop_options = lossy;
    loop_options.loop_tick = 10;
    count = sizeof(looped) / sizeof(*looped);
    assert(!afx_c_simplify_events(looped, &count, &loop_options));
    assert(count == 2 && looped[1].tick == 10);
    /* Zero tolerances keep every event, and still sort. */
    afx_c_event_t unsorted[] = {PATCH_EVENT(5, 0, DIRECT, 0x0805, 0), PATCH_EVENT(1, 0, DIRECT, 0x0805, 0)};
    const afx_c_simplify_t lossless = {.loop_tick = AFX_C_NO_LOOP};
    count = 2;
    assert(!afx_c_simplify_events(unsorted, &count, &lossless));
    assert(count == 2 && unsorted[0].tick == 1 && unsorted[1].tick == 5);
    /* Bad input is refused. */
    afx_c_event_t stray[] = {PATCH_EVENT(0, AFX_MAX_FLOW_CHANNELS, DIRECT, 0x0805, 0)};
    count = 1;
    assert(afx_c_simplify_events(stray, &count, &lossy) == -1);
    assert(afx_c_simplify_events(NULL, &count, &lossy) == -1);
    assert(afx_c_simplify_events(stray, NULL, &lossy) == -1);
    assert(afx_c_simplify_events(stray, &count, NULL) == -1);
#undef NOTE_EVENT
#undef PATCH_EVENT
    return 0;
}
