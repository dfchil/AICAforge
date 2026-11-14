#ifndef AICAFLOW_COMPILE_C_H
#define AICAFLOW_COMPILE_C_H

#include <stdint.h>

#include <aicaflow/format.h>
#include <aicaflow/limits.h>

typedef struct {
    uint32_t start_tick, end_tick;
    uint8_t key, velocity;
    /* MIDI bank/program are resolved at NOTE-on. They are authoring data only:
     * the emitted AFX contains setup indices, never MIDI lookups. */
    uint8_t bank_msb, bank_lsb, program;
    /* Zero selects a zone by MIDI attributes. Nonzero is a one-based resolved
     * zone index used by importers which expand layered SoundFont notes. */
    uint16_t setup_index;
    /* Zero uses the compiler's ordinary MIDI-velocity curve. Importers may
     * lower a source instrument's velocity law to an exact AICA MIX word. */
    uint16_t mix;
    /* KEYOFF time. Zero means end_tick. A later end_tick reserves the AICA
     * channel through a release tail without adding a runtime command. */
    uint32_t release_tick;
    /* MIDI controller state sampled at NOTE-on. It is authoring input only:
     * SoundFont lowering turns it into ordinary NOTE pitch/MIX values.
     * `controllers_valid` keeps hand-authored C notes source-compatible. */
    uint64_t controller_state;
    /* The complete CC snapshot and pressure values are likewise offline-only.
     * SF2 pmod/imod graphs may reference any MIDI controller at NOTE-on; no
     * controller or SoundFont state reaches AICA. */
    uint8_t controllers[128], poly_pressure, channel_pressure, pitch_sensitivity;
    /* Source identity is retained only while authoring.  It lets deterministic
     * offline performance transforms remain stable when a score is rebuilt. */
    uint32_t source_id, source_track, source_order, source_tick;
    int16_t attenuation_offset_centibels;
} afx_c_note_t;

typedef struct {
    uint8_t *afb, *afx, *afc, *afv;
    uint32_t afb_bytes, afx_bytes, afc_bytes, afv_bytes;
    /* A song (afx_c_compile_song): the AFM, which takes the place of the AFX. */
    uint8_t *afm;
    uint32_t afm_bytes;
    /* Song tail events the per-tick budget moved later at a new-play join,
     * and the ticks they moved in all. */
    uint32_t moved_events, moved_ticks;
} afx_c_output_t;

/* Build checkpoint-only seek data from final AFX bytes, at configurable authored
 * intervals (rounded up to a tick). out->afc must be NULL; other outputs stay unchanged. */
#define AFX_C_CHECKPOINT_SECONDS 10u
int afx_c_build_seek(afx_c_output_t *out, uint32_t seconds);
/* Validate the final stream and store its peak command/register-write budget. */
int afx_c_set_work_profile(uint8_t *afx, uint32_t bytes);

/* One explicit AICA sample. `loop_end` is inclusive, as in AICA's LEA
 * register. `frames` is decoded PCM frame count even for ADPCM; `bytes` is
 * the contiguous AFB payload size. A non-looping source still has valid loop
 * bounds but clears looping. */
typedef struct {
    const uint8_t *data;
    uint32_t bytes, frames;
    uint8_t format, root_key, loop;
    uint16_t loop_start, loop_end;
    int16_t tuning_cents;
    /* Zero means the normal 44.1 kHz AICA base rate. */
    uint32_t sample_rate;
} afx_c_sample_t;

typedef struct {
    afx_c_sample_t sample;
    uint8_t key_min, key_max, velocity_min, velocity_max;
    uint8_t bank_msb, bank_lsb, program;
    /* AICA DSP-send register byte: IMXL in the high nibble, ISEL in low. */
    uint8_t dsp_send;
    /* Offline importers can lower an instrument's static controls straight
     * into the setup template.  These are ordinary AICA register words, not
     * another runtime abstraction. */
    uint32_t setup_mask;
    uint16_t setup[AFX_FIELD_COUNT];
} afx_c_zone_t;

/* A resolved control-stream event for importers whose source has live register
 * writes (for example MultiPCM). `fields` is indexed by AFX_FIELD_*; only
 * bits selected by `mask` are emitted.  NOTE setup indices are zero-based. */
typedef struct {
    uint32_t tick, order, mask;
    uint16_t setup;
    uint8_t opcode, channel;
    uint16_t fields[AFX_FIELD_COUNT];
} afx_c_event_t;

/* Normalize source-specific NOTE events into a compact setup dictionary.
 * Input NOTE events must carry PITCH and MIX; the function returns owned
 * event/template arrays for the common emitter.  Non-NOTE events are copied.
 */
int afx_c_optimize_events(const afx_c_event_t *events, uint32_t count,
                          const afx_c_zone_t *zones, uint32_t zone_count,
                          afx_c_event_t **out_events, afx_c_zone_t **out_zones,
                          uint32_t *out_zone_count);

/* Lossy simplification of a register trace before emission, for importers that record every
 * change a source driver made (a console recording, say). Within one channel, a PATCH value is
 * dropped while it stays within a tolerance of the last value kept, so the AICA keeps a value at
 * most that far from the source's:
 * - `pan_tolerance`: DIRECT changes that only move pan (DIPAN, sign-magnitude) by at most this many
 *   steps of 15 per side; changes of the other DIRECT bits are kept.
 * - `level_tolerance`: MIX changes that only move attenuation (TL, the high byte, 0.375 dB steps)
 *   by at most this many steps. A change to full attenuation (0xFF) is always kept.
 * A NOTE sets a channel's state from its own values; a field it leaves to its setup template is
 * unknown, so the next change of it is kept, as is every channel's first change from the loop
 * tick on. A PATCH left with no fields is removed. Zero
 * tolerances keep everything. `*count` is updated; events end up in (tick, order) order. */
#define AFX_C_NO_LOOP UINT32_MAX
typedef struct {
    uint8_t pan_tolerance;
    uint8_t level_tolerance;
    /* A looping song's loop tick (AFX_C_NO_LOOP for none), the start of the body a continue
     * join returns to: playback reaches it a second time with the state the end of the song
     * left, so every channel's state is unknown again from there. */
    uint32_t loop_tick;
} afx_c_simplify_t;
int afx_c_simplify_events(afx_c_event_t *events, uint32_t *count, const afx_c_simplify_t *options);

/* Apply a source's explicit NOTE cluster policy before common emission.
 * `cluster_limit` is commands per control tick (1..38); note lifetimes move
 * together by whole ticks, so no timing is silently shortened. */
int afx_c_schedule_notes(afx_c_note_t *notes, uint32_t count, uint8_t cluster_limit);

/* Allocate AICA voices for a chronologically sorted score.  Importers with
 * source-side automation use the returned channel map to lower live controls
 * into PATCH commands while retaining the common allocator. */
int afx_c_assign_channels(const afx_c_note_t *notes, uint32_t count,
                          uint8_t *out_channels, uint32_t *out_channel_count);

/* Every note must select exactly one key range. The AFB retains all samples
 * in source-zone order, 32-byte aligned, sharing identical data pointers and
 * byte lengths. Only the AFX setup dictionary is pruned/reordered by usage;
 * its relocations still refer to the stable bank layout used by AFI indices. */
int afx_c_compile_zones(const afx_c_note_t *notes, uint32_t count,
                        uint32_t tick_rate, const afx_c_zone_t *zones,
                        uint32_t zone_count, afx_c_output_t *out);
/* As afx_c_compile_zones, for a song that repeats from loop_tick (an authored
 * tick before the end): an AFM of an intro section before loop_tick and a body
 * after it, both joined by AICAflow's continue join, so a voice sounding at
 * the loop keeps sounding, as a LOOP flow's did; the order is intro, body,
 * looping to the body. A loop tick of zero makes the body the whole song. */
int afx_c_compile_zones_song(const afx_c_note_t *notes, uint32_t count,
                             uint32_t tick_rate, uint32_t loop_tick, const afx_c_zone_t *zones,
                             uint32_t zone_count, afx_c_output_t *out);

/* Assemble a bank-bound AFX from already-resolved NOTE, PATCH and KEYOFF
 * events. `duration_ticks` is the source's final control tick.
 * A tick holding more than AFX_EXECUTION_BUDGET_* work is not an error: the
 * events that do not fit move, in order, to the following ticks.
 * An optional final PARK at duration_ticks emits a controlled flow without
 * song-only seek/visual sidecars, including a NOTE/PARK at tick zero. */
int afx_c_compile_events(const afx_c_event_t *events, uint32_t count,
                         uint32_t duration_ticks, uint32_t tick_rate,
                         const afx_c_zone_t *zones, uint32_t zone_count,
                         afx_c_output_t *out);
/* A song (AICAflow's AFM, firmware ABI 14): sections played in an order.
 * A section's events are in ticks from its start, on its own local channels,
 * and may include AFX_OP_FREE (give the channel's voice back to the pool).
 * At body_ticks its NEXT moves the song to the next order position:
 * - AFX_JOIN_CONTINUE: the same executor goes on into the next section with
 *   its voices; no event may come after body_ticks, and end_ticks equals it;
 * - AFX_JOIN_NEW: the next section starts on the song's other executor with
 *   voices of its own, while this one plays its tail (events after
 *   body_ticks) to END at end_ticks or after its last event.
 * Events at body_ticks itself play before the NEXT. After the last order
 * position the song goes on at loop_index, or with AFX_C_SONG_ENDS ends.
 *
 * Every section shares the setup table (zones, as afx_c_compile_events takes
 * them). A tick's work beyond the budget moves later as in a flow; a body
 * event moved past body_ticks is refused. At a new-play join, the tail's
 * events move later by whole ticks wherever they and the next sections'
 * first events would together pass the budget; out->moved_events and
 * moved_ticks say how many and how far. A song AICAflow would refuse (a tail
 * still playing at the next new-play join, say) is refused.
 * out gets the AFB and the AFM (afm, afm_bytes); no AFC, as AICAflow cannot
 * seek a song, and no AFV. */
typedef struct {
    const afx_c_event_t *events;
    uint32_t count;
    uint32_t body_ticks; /* NEXT: the next section starts here. */
    uint32_t end_ticks; /* END; at least body_ticks. */
    uint32_t join; /* AFX_JOIN_NEW or AFX_JOIN_CONTINUE. */
} afx_c_section_t;
#define AFX_C_SONG_ENDS UINT32_MAX
int afx_c_compile_song(const afx_c_section_t *sections, uint32_t section_count,
                       const uint16_t *order, uint32_t order_count, uint32_t loop_index,
                       uint32_t tick_rate, const afx_c_zone_t *zones, uint32_t zone_count,
                       afx_c_output_t *out);
/* As afx_c_compile_song, with a lane per local channel (AICAflow lanes): lanes
 * holds lane_channels entries, at least the channels the song uses. */
int afx_c_compile_song_lanes(const afx_c_section_t *sections, uint32_t section_count,
                             const uint16_t *order, uint32_t order_count, uint32_t loop_index,
                             const uint8_t *lanes, uint32_t lane_channels,
                             uint32_t tick_rate, const afx_c_zone_t *zones, uint32_t zone_count,
                             afx_c_output_t *out);

/* Compile a resolved timeline against one explicit sample. All output sidecars
 * are allocated with the AFB/AFX pair and released by afx_c_output_free(). */
int afx_c_compile_sample(const afx_c_note_t *notes, uint32_t count,
                         uint32_t tick_rate, const afx_c_sample_t *sample,
                         afx_c_output_t *out);

/* Compile a resolved monophonic/polyphonic note timeline using the built-in
 * sine source. The caller owns out through afx_c_output_free(). */
int afx_c_compile_sine(const afx_c_note_t *notes, uint32_t count,
                       uint32_t tick_rate, afx_c_output_t *out);
/* Make a VIZ1 sidecar from the final AFX command stream.  This deliberately
 * follows emitted NOTE, PATCH and KEYOFF commands rather than a parallel MIDI
 * approximation, so it remains aligned with offline AFP transforms. */
int afx_c_visualize(const uint8_t *afx, uint32_t bytes,
                    uint8_t **out_visual, uint32_t *out_bytes);
void afx_c_output_free(afx_c_output_t *out);

#endif
