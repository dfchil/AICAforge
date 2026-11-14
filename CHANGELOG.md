# Changelog

## Unreleased

Changes since 0.1.0-rc1. The entries up to 2026-10-09 were reconstructed from
the commit history.

### Added

- Songs (AICAflow AFM, firmware ABI 14): `afx_c_compile_song()` and
  `afx_c_compile_song_lanes()` compile sections, their joins (continue or new
  play), an order and a loop index into an AFB and an AFM with one shared
  setup table. At a new-play join, tail events move later where they and the
  next section's head would pass the per-tick budget, and the output reports
  how many moved and how far. `afx_compile --loop-tick N` now writes a
  continue-only song (intro and body) instead of a LOOP flow, through
  `afx_c_compile_zones_song()`. `afx_scene` manifests take `song NAME file.afm`
  entries. Flows without a loop compile byte for byte as before. Needs the
  pinned AICAflow with songs (format API 6).

### Removed

- `afx_c_compile_events_loop()` and `afx_c_compile_zones_loop()`, and LOOP in
  `afx_profile`: AICAflow format API 6 has no LOOP.

- `afx_scene manifest.afsm` plans AICAflow AFS scene files: the banks, flows,
  slots and DSP ring of each scene with their places in the scene's AICA
  block, laid out with the runtime's rounding and checked with the loader's
  `afx_scene_validate()`. It reports each scene's bytes and the headroom of
  the permanent scene plus each level (plus the two largest levels when
  transitions overlap), checks that every flow finds its banks in its scene or
  the permanent one, and writes nothing and exits non-zero when something does
  not fit. Needs the pinned AICAflow with AFS (format API 5).
- `afx_bank --merge --shared core.afb` (up to three shared banks): a sample
  whose bytes a shared bank already holds, at a 32-byte boundary, plays from
  that bank instead of being copied into the merged bank. A flow that then
  plays from more than one bank gets AICAflow's `AFX_FLAG_BANKS`: the bank
  slot in its sample addresses, the shared banks' identities in its bank list
  and a bank table after its stream. A flow whose samples all lie in one bank
  binds that bank as before, and the merged bank is not written when every
  sample is shared. AFC sidecars move with their setups. Needs the pinned
  AICAflow with `AFX_FLAG_BANKS` (format API 4, firmware ABI 13).
- Looping songs: `afx_compile --loop-tick N`, `afx_c_compile_zones_loop()` and
  `afx_c_compile_events_loop()` end the stream with AICAflow's `LOOP` back to
  the instruction boundary at tick N, with `AFX_FLAG_LOOP` and `loop_tick` in
  the header and the loop's joined burst in the work profile. `afx_profile
  apply` carries the loop through its rewrite, and the research decoder reads
  it. Needs the pinned AICAflow with `AFX_OP_LOOP` (firmware ABI 9). The CSeq
  importer does not emit it yet.
- The compiler and `afx_profile` pick compact encodings through AICAflow's
  `afx_compact_opcode()`, so a PATCH of only ENV_AD, or of ENV_AD and the
  level, is written as `PATCH_ENV` (4 bytes) or `PATCH_ENV_LEVEL` (6 bytes)
  instead of a full PATCH (10 or 12). The research decoder reads both. Needs
  the pinned AICAflow with those opcodes (format API 3, firmware ABI 10).
- `afx_n64 --sfx-adpcm` stores one-shot SFX samples as ADPCM at the source
  rate, at half the size of PCM8.
- `afx_c_simplify_events()` thins a recorded register trace before emission,
  for importers that capture every change a driver made. A PATCH of pan or
  level is dropped while it stays within a tolerance of the last value kept.
  Lossy, and off unless an importer asks for it.

### Changed

- Compiled flows carry an authored work profile, which lets the AICAflow
  runtime skip stream validation for trusted assets.
- A tick holding more than 38 commands or 171 register writes is no longer an
  error: the compilers move the surplus, in order, into the following ticks.
- The tools build with GCC as well as Clang (`make CC=gcc`).
- The pinned AICAflow SDK is updated for trusted asset validation.

### Fixed

- `afx_n64` mapped CC 91 (libultra's fxMix) linearly onto IMXL's 3 dB steps,
  so reverb sends came out 15–25 dB below the N64's: 64 gave −21 dB instead
  of −3 dB, and 1–4 gave no send at all. It now follows libultra's
  equal-power wet curve, `sin(π/2 · fx/127)`, rounded to the nearest IMXL
  step, with any nonzero value kept at IMXL 1 or above. DKR's music sends
  get louder; flows compiled before this keep the old sends until rebuilt.
- `afx_c_optimize_events()` sized its template array by the zone count, but a
  zone gets a template for each setup variant used often enough, so a source
  with more variants than zones wrote past the array. SM64's songs reach 65
  templates from 13 zones. The array is now sized by the variant count.
  Output is unchanged wherever the old size was enough (DKR's 64 songs: never
  more templates than zones).
- SoundFont zones without a `sampleModes` generator are one-shots. They were
  looped whenever the sample header carried loop points. This changes the
  output for fonts that relied on it, including the Grieg song in AICAflow's
  music player example.
- A SoundFont local zone's coarse or fine tuning replaces the instrument's
  global zone value instead of being added to it.
- Pitch bend uses the channel's RPN 0 range instead of a fixed two semitones.
- A zero-length MIDI note no longer stays open until the end of the file, and
  a note shorter than one control tick no longer fails the MIDI or CSeq
  import. Both sound for one control tick.
- `afx_profile apply` stamps the work profile the rewritten stream really
  has. Lanes sharing a tick were counted one at a time.
- `afx_profile` reports an unreadable input instead of crashing.
- `afx_bank` rejects a source or song path too long for its map entry instead
  of overflowing it.
- `afx_n64` checks its 16-bit zone number, and `afx_compile` reports a failed
  write.
- Seek checkpoints are regenerated after a profile transform, and the
  reserved checkpoint header word is initialised.

### Known limitations

- `afx_profile apply` still rejects a lane that pushes its tick over the
  limit, rather than moving it.

## 0.1.0-rc1 — 2026-10-05

- Standalone native MIDI/SF2/PCM, N64 CSeq/ALBank and MultiPCM VGM/VGZ tools.
- Shared-bank building and merging, performance profiles and visualisation output.
- Pinned AICAflow SDK for format definitions, validation and compatibility tests.
- Task-based documentation for authoring, formats, resource budgets and tests.
- DSP listener inputs in one shared bank, including correctly pitched Wilhelm
  and a slow modulation source with audio headroom.

### Migration from AICAflow authoring

Run `make dependencies`, then `make`; executables are written to `build/`.
Use `make check` for authoring tests and `make compatibility-check` for SDK
integration. See [Testing](docs/testing.md) for test dependencies.

The SDK dependency is non-recursive. `make update-dependencies` explicitly
updates it; ordinary builds retain the recorded revision. Runtime output
versions are AFB 1, AFX 7, AFC 1 and AFI 1.

This candidate is distributed as source; build the native tools locally.
