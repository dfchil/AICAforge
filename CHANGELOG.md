# Changelog

## Unreleased

Changes since 0.1.0-rc1. The entries up to 2026-10-09 were reconstructed from
the commit history.

### Added

- Looping songs: `afx_compile --loop-tick N`, `afx_c_compile_zones_loop()` and
  `afx_c_compile_events_loop()` end the stream with AICAflow's `LOOP` back to
  the instruction boundary at tick N, with `AFX_FLAG_LOOP` and `loop_tick` in
  the header and the loop's joined burst in the work profile. `afx_profile
  apply` carries the loop through its rewrite, and the research decoder reads
  it. Needs the pinned AICAflow with `AFX_OP_LOOP` (firmware ABI 9). The CSeq
  importer does not emit it yet.
- `afx_n64 --sfx-adpcm` stores one-shot SFX samples as ADPCM at the source
  rate, at half the size of PCM8.

### Changed

- Compiled flows carry an authored work profile, which lets the AICAflow
  runtime skip stream validation for trusted assets.
- A tick holding more than 38 commands or 171 register writes is no longer an
  error: the compilers move the surplus, in order, into the following ticks.
- The tools build with GCC as well as Clang (`make CC=gcc`).
- The pinned AICAflow SDK is updated for trusted asset validation.

### Fixed

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
