# Authoring

The supported offline authoring path is native C. It converts a MIDI timeline
and explicit raw-sample zone map into one AFB and a bank-bound AFX, then writes
matching AFC seek and AFV visualizer sidecars beside the AFX.

Mappings select source samples and their explicit AICA sample coding
(`pcm16`, `pcm8`, `adpcm`, or `auto`). A `.afp` performance profile is an offline-only description of
register-level changes and DSP use: it produces a new AFX but never changes NOTE or KEYOFF
timing. Use the music source for timing and use `.afp` for timbre,
articulation and room treatment. The exact file roles and binary layouts are in
[Assets and sidecars](specs/assets.md); this guide is the human workflow for
creating them.

`make compiler` builds the host tool. It reads the note and tempo subset of a
Standard MIDI file and emits strict AFB/AFX/AFC/AFV assets. For example:

```sh
make compiler
build/afx_compile song.mid song.afb song.afx
# Or use one raw, little-endian PCM16 source (root key 69):
build/afx_compile song.mid instrument.pcm song.afb song.afx
```

Every native importer follows one pipeline: source parsing produces an
unoptimized list of PCM zones plus `NOTE`, `PATCH`, and `KEYOFF` events; the
shared optimizer chooses reusable setup templates; the shared emitter writes
AFB/AFX/AFC/AFV. Source parsers must not write AFX bytes themselves. This is
also used by `afx_n64`, which reads N64 B1 ALBank + S1 CSeq directly (without
a MIDI intermediate):

```sh
build/afx_n64 audio_control.bin audio_table.bin sequences.bin 7 song.afx
```

ALBank key/velocity splits, envelopes, pan, CC91 send, VADPCM and RAW16
samples are all lowered offline to ordinary zones and AICA register values.
An infinite CSeq source loop is represented by its first pass because an AFX
flow is deliberately finite.

The current PCM input is raw little-endian PCM16 at AICA's 44.1 kHz playback
rate and uses MIDI key 69 as its root. This short form is intentionally a
one-shot source; resampling and register-level articulation remain the next C
authoring layer.

The C compiler also accepts a small text zone map for key-split instruments:

```text
# key_min key_max root_key loop_start loop_end format pcm_path
0 65 48 -1 -1 pcm8 bass.pcm
66 127 72 0 1023 auto lead_cycle.pcm
```

`-1 -1` means one-shot; other loop bounds are inclusive PCM frame indices.
Every MIDI note must match exactly one zone. The compiler emits one 32-byte
aligned AFB payload. It greedily shares same-sample setup templates and moves
only differing static register words onto the affected NOTE, so a zone split
does not automatically cost another 36-byte setup.

The raw AICA address field reaches 65,535 decoded frames, but that is not the
authoring limit: a one-shot reserves a 256-frame silent safety tail and is
therefore capped at 65,279 frames; a looped sample is capped at 65,500 frames
to retain rounding room around its inclusive loop end.

```sh
build/afx_compile song.mid --zones instrument.zones song.afb song.afx
```

For SoundFont input, no Python package is needed. The C reader uses each
note's MIDI bank/program to select SF2 preset and instrument zones. The
requested encoding is explicit; `auto` chooses the smallest format passing the
same deterministic quality gate as zone maps (and conservatively skips ADPCM
for looped sources), then uses the established PCM8 baseline. PCM16 remains
an explicit mapping choice when a source needs it:

```sh
build/afx_compile song.mid --sf2 auto GeneralUser.sf2 song.afb song.afx
```

The native reader lowers the common SF2 instrument controls offline: volume
attack/decay/sustain/release, initial attenuation, pan, optional source reverb
send, static filter and filter-envelope controls, plus the standard
velocity-to-level curve. They become ordinary AICA setup words and NOTE mix
values; neither the SH4 nor ARM7 parses SF2 data.

Several songs can share one AFB. An `.afbm` text map declares any number of
SoundFont sources, optionally selects `stereo`, `left`, or `right` from linked
stereo samples, and routes each MIDI bank/program pair to a source preset.
The stable map prefix is followed by named conversion options, so adding a
policy never changes the meaning of an old column. This also lets one song
combine several SoundFonts while runtime still sees exactly one bank:

```text
source gm        soundfonts/GeneralUser.sf2
source orchestra soundfonts/orchestra.sf2 stereo
map * 0 0  gm        0 0  auto rate=22050
map * 0 42 orchestra 0 42 pcm16 rate=44100 filter=static gain=fluidsynth2
song title_theme midi/title_theme.mid 1000 750
song field_theme midi/field_theme.mid
```

```sh
build/afx_bank library.afbm output music.afb
```

The result is one `music.afb`, plus `title_theme.afx/.afc/.afv` and
`field_theme.afx/.afc/.afv`. AFX remains sample-free; every flow is bound to
the generated bank identity at load time.

The available per-map options are `rate=<hz>`, `filter_offset=<cents>`,
`filter=envelope|static|none`, `gain=standard|fluidsynth2`,
`gain_bias=<centibels>`, `envelope=sf2|fixed`,
`source_pan=apply|ignore`, `source_reverb=apply|ignore`, `dsp_send=<0..255>`,
`modulators=apply|ignore`,
`midi_channel=<0..15>`, `direct=<AICA-word>`, `lfo=<rate>,<pitch-depth>,<amplitude-depth>` and
`loop_ms=<20..2000>`. `dsp_send` and `source_reverb=apply` are mutually
exclusive. `modulators=apply` is the default: linear SF2 graph inputs are
sampled at NOTE-on and folded into that note's existing setup/NOTE data.
Curved and time-varying graph inputs remain deliberately unsupported rather
than being guessed; use an AFP lane when an authored AICA PATCH is wanted.
Defaults are SF2 envelope/filter, standard gain, source pan and modulators
applied, and source reverb ignored. The map, not an AFP, owns all of these
choices because they can change the bank payload or base setup.

`midi_channel` is an optional source selector, not an AICA output channel.
It is useful when a score reuses a MIDI program number for separate parts,
such as melodic strings and percussion. A channel-specific map wins over the
generic rule for the same song and program; a song-specific generic rule wins
over `*`.

For a declared historic rendition, `humanize <song> <seed> <level-centibels>
<shorten-ms> <lfo-rate-steps>` may follow its `song` line in the AFBM. It is a
deterministic offline lowering step: it modifies NOTE level and KEYOFF timing,
but adds no SH4 or ARM7 feature. New rhythmic edits should instead live in the
MIDI source; the directive is retained solely where it captures an already
approved rendering. Native parity currently requires `lfo-rate-steps=0`.

## Performance profiles

`build/afx_profile init` creates an editable JSON `.afp` file from an
already-built AFX. It binds the sidecar to that exact base with SHA-256 and
lists every NOTE event by its stable `{tick, ordinal, channel}` identity. The
initial `all-notes` template makes the common case visible before an editor
splits it into named tone templates.

```sh
build/afx_profile init song.afx song.afp room 112
# edit song.afp offline
build/afx_profile apply song.afx song.afc song.afp song-performance.afx song-performance.afc
```

The current C applier implements the shared DSP scene and send in the `dsp`
section (`dry`, `room`, `room_warm`, or `room_large`). `tempo_q8_8` is an
optional whole-flow rate (`256` is authored speed) applied by SH4 at activation;
it does not move individual notes. The applier writes sends into the derived
AFX, updates its control identity and rewrites the matching AFC header. The
generated tone inventory is the stable input for the targeted template rewrite;
no profile is interpreted by ARM7. An old profile intentionally fails after its
AFX source changes rather than silently applying to a different set of notes.

To create a map instead of writing the initial MIDI-program mapping by hand:

```sh
build/afx_bank --create-map library.afbm GeneralUser.sf2 \
  title_theme midi/title_theme.mid field_theme midi/field_theme.mid
```

The optional fourth `song` field reserves a release tail in milliseconds. It
keeps that AICA channel unavailable after its musical `KEYOFF`, so an SF2
release can fade naturally while a following note is scheduled onto another
channel. It is entirely offline: the emitted AFX still contains only ordinary
`NOTE` and `KEYOFF` commands.

The initial C reader accepts timing and note events (including running status,
tempo, note-off, sustain pedal and all-notes-off). It deliberately shares the
public wire validator with the driver. The former Python implementation is
retained under `tools/research/` only as a reference and test aid; it is not
the format or runtime authority.
