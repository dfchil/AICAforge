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
build/afx_compile_c song.mid song.afb song.afx
# Or use one raw, little-endian PCM16 source (root key 69):
build/afx_compile_c song.mid instrument.pcm song.afb song.afx
```

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
aligned AFB payload and one setup/relocation per zone:

```sh
build/afx_compile_c song.mid --zones instrument.zones song.afb song.afx
```

For SoundFont input, no Python package is needed. The C reader uses each
note's MIDI bank/program to select SF2 preset and instrument zones. The
requested encoding is explicit; `auto` chooses the smallest format passing the
same deterministic quality gate as zone maps (and conservatively skips ADPCM
for looped sources), then uses the established PCM8 baseline. PCM16 remains
an explicit mapping choice when a source needs it:

```sh
build/afx_compile_c song.mid --sf2 auto GeneralUser.sf2 song.afb song.afx
```

Several songs can share one AFB. An `.afbm` text map declares any number of
SoundFont sources, routes each MIDI bank/program pair to a source preset, and
states that mapping's format. This also lets one song combine samples from
several SoundFonts while runtime still sees exactly one bank:

```text
source gm        soundfonts/GeneralUser.sf2
source orchestra soundfonts/orchestra.sf2
map * 0 0  gm        0 0  auto
map * 0 42 orchestra 0 42 pcm16
song title_theme midi/title_theme.mid
song field_theme midi/field_theme.mid
```

```sh
build/afx_bank_c library.afbm output music.afb
```

The result is one `music.afb`, plus `title_theme.afx/.afc/.afv` and
`field_theme.afx/.afc/.afv`. AFX remains sample-free; every flow is bound to
the generated bank identity at load time.

## Performance profiles

`build/afx_profile_c init` creates an editable JSON `.afp` file from an
already-built AFX. It binds the sidecar to that exact base with SHA-256 and
lists every NOTE event by its stable `{tick, ordinal, channel}` identity. The
initial `all-notes` template makes the common case visible before an editor
splits it into named tone templates.

```sh
build/afx_profile_c init song.afx song.afp room 112
# edit song.afp offline
build/afx_profile_c apply song.afx song.afc song.afp song-performance.afx song-performance.afc
```

The current C applier implements the shared DSP scene and send in the `dsp`
section (`dry`, `room`, `room_warm`, or `room_large`). It writes those sends
into the derived AFX, updates its control identity and rewrites the matching
AFC header. The generated tone inventory is the stable input for the targeted
template rewrite; no profile is interpreted by SH4 or ARM7. An old profile
intentionally fails after its AFX source changes rather than silently applying
to a different set of notes.

To create a map instead of writing the initial MIDI-program mapping by hand:

```sh
build/afx_bank_c --create-map library.afbm GeneralUser.sf2 \
  title_theme midi/title_theme.mid field_theme midi/field_theme.mid
```

The initial C reader accepts timing and note events (including running status,
tempo, note-off and all-notes-off). It deliberately shares the public wire
validator with the driver. The former Python implementation is retained under
`tools/research/` only as a reference and test aid; it is not the format or
runtime authority.
