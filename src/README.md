# Native authoring tools

Build the host compiler from the repository root:

```sh
make compiler
build/afx_compile song.mid --zones instrument.zones song.afb song.afx
```

See [the tool inventory](../README.md) for every executable and build
requirement, [Authoring](../../docs/authoring.md) for end-to-end workflows,
and [Assets and sidecars](../../docs/specs/assets.md) for the authoritative
schemas. The zone-map mode above is a low-level raw-PCM input; current AFBM
`source` entries accept SF2, not standalone PCM.

`afx_profile` is the companion offline performance step. It binds an editable
`.afp` JSON sidecar to one exact base AFX and lowers its global, setup-template
and per-note timbre/DSP-send choices to an ordinary derived AFX plus matching
AFC seek index:

```sh
build/afx_profile init song.afx song.afp room 112
build/afx_profile apply song.afx song.afc song.afp song-performance.afx song-performance.afc
build/afx_profile inventory song.afx
build/afx_compile --visual song-performance.afx song-performance.afv
build/afx_profile describe song.afx song.afp
```

`export-lanes reference.afx base.afx profile.afp preset send [tempo_q8_8]`
turns an ABI-7 reference flow's sustained `PATCH` commands into an editable
profile bound to `base.afx`. It follows the source NOTE order, rather than
copying allocator channel numbers, so it is useful when a rebuilt bank changes
voice allocation. Use `apply` to compile those profile lanes into ordinary
AFX commands; no profile is interpreted at runtime.

`inventory` is a read-only selector list for an editor or a human author. It
keeps the profile itself compact: a template is not repeated for every note it
affects. See [the AFP specification](../../docs/specs/assets.md#afp--performance-profile)
for the inheritance order and supported register fields.
`init` creates compact defaults and empty collections. `describe` prints
the preset/send/tempo for build-time player metadata; loading a derived AFX
does not itself install a DSP scene or apply its chosen tempo. A changed
profile currently regenerates an initial-only AFC checkpoint; seeking then
replays on SH4. The `apply` command requires a matching input AFC.

The compiler emits `song.afb`, `song.afx`, `song.afc`, and `song.afv` together.
The AFB is one 32-byte-aligned payload; the AFX only contains setup registers
and control commands. The shared C emitter greedily reuses a sample's most
common setup and puts only divergent envelope, filter, LFO, DSP-send or pan
words on the affected NOTE. This applies equally to raw zones, MIDI/SF2 and
bank-map builds. Importers first lower into the same unoptimized zone +
NOTE/PATCH/KEYOFF representation, then use that optimizer and emitter; it
never changes audible register state. AFC and AFV are optional at runtime,
but deterministic outputs of this authoring step.

`afx_n64` is the equivalent direct path for Nintendo 64 `B1` ALBank control
data, its sample table, and an `S1` compact-sequence file. It does **not**
round-trip through MIDI: CSeq becomes the same neutral note timeline, ALBank
becomes the same raw zones, and the common optimizer/emitter writes the final
assets. This makes the N64 path a reference for future source importers
without adding an N64-specific AFX variant.

```sh
build/afx_n64 audio_control.bin audio_table.bin sequences.bin 7 title_theme.afx
```

The command writes `title_theme.afb`, `.afx`, `.afc`, and `.afv`. It retains
the AL envelope, key/velocity region, root key, detune, pan, CC91 send and
VADPCM/RAW16 sample meaning offline. Infinite CSeq loops end at their first
boundary because the emitted AFX remains finite.

The same C program lowers one ALBank sound-chain using DKR's source chain
semantics. Other N64 games need their own source compatibility established:

```sh
build/afx_n64 --sfx audio_control.bin audio_table.bin 563 collect_item.afx
```

It writes the matching `collect_item.afb` and `.afx`. ID 563 is a one-based
raw ALInstrument root, not a logical `SOUND_*` enum. Sustained source decay is
marked `PARK`/controlled so the game, rather than an invented duration, owns
its lifetime. SFX do not need an AFC seek sidecar. `afx_bank --merge` accepts
such AFX/AFB pairs and rewrites them into one shared SFX bank.
An [AFSFX map](../../docs/specs/afsfx.md) can drive application grouping, but
neither native tool currently reads it; that reader is in DKR. OoT AudioSeq
is separate research tooling, not a supported `afx_n64` mode.

The SFX path tries the source rate and lower 16/11.025/8 kHz candidates,
then selects the smallest coding
whose decoded, resampled result meets 30 dB full-sample and 24 dB attack SNR
against the original. Loops use PCM8 or PCM16, never ADPCM. Negative N64 decay
means no AICA decay; finite components still get KEYOFF even in a chain with
a sustained component. Each component's template retains its NOTE's baseline
pitch and mix so SH4 can scale live controls correctly.

`afx_vgm` follows that identical final pipeline for Sega MultiPCM VGM/VGZ
captures. Its source register writes become raw NOTE/PATCH/KEYOFF events; the
same optimizer handles its setup dictionary and the same emitter writes the
four assets.

```sh
build/afx_vgm track.vgz track.afx
# Optional explicit coding/gain choices; default is PCM8 at -6.4 dB:
build/afx_vgm track.vgz track.afx --gain-db -6.4 --adpcm
```

Only Sega MultiPCM is supported, not arbitrary VGM chips or YM2612/PSG.

Zone maps are deliberately small text files:

```text
# key_min key_max root_key loop_start loop_end format pcm_path
0 65 48 -1 -1 pcm8 bass.pcm
66 127 72 0 1023 auto lead_cycle.pcm
```

PCM is little-endian signed PCM16 at AICA's native 44.1 kHz rate. `-1 -1`
means one-shot; other loop endpoints are inclusive decoded sample frames.
Every zone explicitly selects `pcm16`, `pcm8`, `adpcm`, or `auto`. `auto`
uses the smallest representation that passes the offline gate: ADPCM needs at
least 30 dB whole-sample and 24 dB attack SNR; otherwise it uses PCM8, the
project's proven baseline. PCM16 is an explicit quality/space decision in the
map. `auto` deliberately skips ADPCM for looped sources, because its predictor
seam needs a real hardware capture check. The gate is deterministic, and
affects only the generated AFB, never runtime behavior.

The compiler does not use the loop-end register's full 65,535 range. One-shots
reserve a 256-frame silent safety tail and stop at 65,279 frames; looped
sources stop at 65,500 so inclusive loop-end rounding remains safe.

For declared SoundFont input, the C reader follows MIDI program/bank selection,
SF2 preset/instrument zones, key/velocity ranges, root-key overrides, tuning
and loop mode, then expands intentional SF2 layers before emitting ordinary
AICA setup records:

```sh
build/afx_compile song.mid --sf2 auto GeneralUser.sf2 song.afb song.afx
```

To make one bank shared by several pieces, create an `.afbm` bank map. It is
the editable source of truth: `source` declares any number of SoundFonts (with
an optional `stereo`, `left`, or `right` channel selection), and
each `map` routes a MIDI bank/program pair to an SF2 bank/program, explicitly
chooses that mapping's sample format, and accepts named conversion settings
such as decoded rate, filter policy, gain calibration, envelope policy and
loop trimming. A `song` may set its
offline control tick rate and an optional release-tail duration in milliseconds.
The tail reserves a channel after its musical KEYOFF, so an SF2 release can
finish naturally without a runtime extension. AICA receives the resulting
fixed-rate stream. A song may therefore use several source banks without
creating a second runtime bank type.

```text
source gm        soundfonts/GeneralUser.sf2
source orchestra soundfonts/orchestra.sf2 stereo

# map <song|*> <midi-bank> <midi-program> <source> <sf2-bank> <sf2-program> <format> [key=value ...]
map * 0 0  gm        0 0  auto rate=22050
map * 0 42 orchestra 0 42 pcm16 rate=44100 filter=static gain=fluidsynth2

song title_theme midi/title_theme.mid 1000 750
song field_theme midi/field_theme.mid
```

```sh
build/afx_bank library.afbm output music.afb
```

This writes `output/music.afb` and `output/<basename>.afx/.afc/.afv` for each
line. Every output AFX is validated against the one AFB identity; it never
contains a fallback sample copy. A song-specific `map title_theme ...` takes
precedence over a `map * ...` default, so separate MIDI files may reuse the
same MIDI program with different source presets. `midi_channel=<0..15>`
optionally narrows a map to one source MIDI channel and wins over the generic
map for that song/program.

`afx_bank --merge` is for source importers that already emitted complete
single-song AFB+AFX pairs, with an optional sibling AFC. It deduplicates their sample payloads into one
AFB and rewrites each flow and seek sidecar to bind to that bank. It is how
DKR's direct CSeq path makes its shared music bank; it adds no runtime format.
Merging is lossless: it preserves sample bytes and coding, loop bounds and
playback parameters. Quality/format decisions belong to the source importer
or bank map, not to the merge operation.
It neither emits AFI nor rebuilds AFV/playlist metadata; those are separate
map-based/player authoring steps.

```sh
build/afx_bank --merge music.afb controls raw/sequence_*.afx
```

Generate an editable starting map from one SoundFont and one or more MIDI
inputs; the generated mappings use `auto` and can be revised afterwards:

```sh
build/afx_bank --create-map library.afbm GeneralUser.sf2 \
  title_theme midi/title_theme.mid field_theme midi/field_theme.mid
```
