# Native authoring tools

Build the host compiler from the repository root:

```sh
make compiler
build/afx_compile_c song.mid --zones instrument.zones song.afb song.afx
```

The compiler emits `song.afb`, `song.afx`, `song.afc`, and `song.afv` together.
The AFB is one 32-byte-aligned payload; the AFX only contains setup registers
and control commands. AFC and AFV are optional at runtime, but deterministic
outputs of this authoring step.

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

For declared SoundFont input, the C reader follows MIDI program/bank selection,
SF2 preset/instrument zones, key/velocity ranges, root-key overrides, tuning
and loop mode, then expands intentional SF2 layers before emitting ordinary
AICA setup records:

```sh
build/afx_compile_c song.mid --sf2 auto GeneralUser.sf2 song.afb song.afx
```

To make one bank shared by several pieces, create an `.afbm` bank map. It is
the editable source of truth: `source` declares any number of SoundFonts, and
each `map` routes a MIDI bank/program pair to an SF2 bank/program and explicitly
chooses that mapping's sample format. A song may therefore use several source
banks without creating a second runtime bank type.

```text
source gm        soundfonts/GeneralUser.sf2
source orchestra soundfonts/orchestra.sf2

# map <song|*> <midi-bank> <midi-program> <source> <sf2-bank> <sf2-program> <format>
map * 0 0  gm        0 0  auto
map * 0 42 orchestra 0 42 pcm16

song title_theme midi/title_theme.mid
song field_theme midi/field_theme.mid
```

```sh
build/afx_bank_c library.afbm output music.afb
```

This writes `output/music.afb` and `output/<basename>.afx/.afc/.afv` for each
line. Every output AFX is validated against the one AFB identity; it never
contains a fallback sample copy. A song-specific `map title_theme ...` takes
precedence over a `map * ...` default, so separate MIDI files may reuse the
same MIDI program with different source presets.

Generate an editable starting map from one SoundFont and one or more MIDI
inputs; the generated mappings use `auto` and can be revised afterwards:

```sh
build/afx_bank_c --create-map library.afbm GeneralUser.sf2 \
  title_theme midi/title_theme.mid field_theme midi/field_theme.mid
```
