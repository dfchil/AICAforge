# Authoring

The supported offline authoring tools are native C. Choose an importer for the
actual source: MIDI/SF2 or raw PCM, N64 CSeq/ALBank, or Sega MultiPCM VGM/VGZ.
All lower into the same AICA register/event representation and emit the same
bank-bound playback model. A game integration can separately group its SFX
with an [AFSFX residency map](specs/afsfx.md).

Mappings select source samples and their explicit AICA sample coding
(`pcm16`, `pcm8`, `adpcm`, or `auto`). A `.afp` performance profile is an offline-only description of
register-level changes and DSP use: it produces a new AFX but never changes NOTE or KEYOFF
timing. Use the music source for timing and use `.afp` for timbre,
articulation and room treatment. The exact file roles and binary layouts are in
[Assets and sidecars](specs/assets.md); this guide is the human workflow for
creating them.

## What to edit

| Desired change | Edit |
| --- | --- |
| Score, rhythm, NOTE/KEYOFF placement | MIDI or the original sequence source |
| Source SoundFont, preset, sample quality/rate, loop trimming, SF2 lowering | AFBM |
| Shared/per-note AICA timbre, sustained PATCH expression, DSP send or overall pace | AFP |
| Which game SFX banks are preloaded together | Application AFSFX map |
| Runtime interaction (engine pitch, position, intensity, STOP) | SH4 code |

Edit source inputs, maps and profiles; regenerate AFB/AFX/AFC/AFV/AFI outputs.
A rebuilt bank needs matching AFX bindings. A changed AFX needs a matching
AFC and updated AFP source binding.

```mermaid
flowchart LR
  midi["MIDI + SF2/PCM policy"] --> ir["Resolved samples/setups + NOTE/PATCH/KEYOFF events"]
  n64["B1 ALBank + S1 CSeq / SFX chain"] --> ir
  vgm["MultiPCM VGM/VGZ"] --> ir
  ir --> optimize["Shared template optimizer"] --> emit["Shared validated emitter"]
  emit --> bank["AFB: encoded samples"]
  emit --> flow["AFX: control image"]
  emit --> sidecars["AFC + AFV for finite music"]
  flow --> profile["Offline AFP rewrite"] --> derived["Derived AFX + matching AFC"]
```

AFP reuses the AFB unchanged. AFI catalogs are additional outputs of map-based
`afx_bank` builds; not every importer produces them. Controlled SFX emit only
AFB/AFX, without song-only seek/visual sidecars.

## Single MIDI and raw PCM

`make compiler` builds the host tools. `afx_compile` reads the note and tempo subset of a
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
AFB/AFX and the applicable sidecars. The optimizer shares repeated register
setups and uses compact NOTE_PL/PATCH_LEVEL encodings without changing
register state or event timing.

Raw PCM input is little-endian PCM16 at 44.1 kHz, with MIDI key 69 as its root.
The short form plays one-shot samples. Use SF2/AFBM for sample-rate and instrument
policies below, and AFP for derived register articulation.

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

The AICA loop-end register reaches frame index 65,535, but that is not the
authoring limit: a one-shot reserves a 256-frame silent safety tail and is
therefore capped at 65,279 frames; a looped sample is capped at 65,500 frames
to retain rounding room around its inclusive loop end.

```sh
build/afx_compile song.mid --zones instrument.zones song.afb song.afx
```

## SoundFonts and bank maps

For SoundFont input, the reader uses each
note's MIDI bank/program to select SF2 preset and instrument zones. The
requested encoding is explicit; `auto` chooses the smallest format passing the
same deterministic quality gate as zone maps: ADPCM requires 30 dB whole-sample
and 24 dB attack SNR; otherwise PCM8 is used. Looped sources use PCM8.
Select PCM16 explicitly for higher sample precision:

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
Each route has named conversion options. A song can combine several SoundFonts
in one bank:

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
The builder also writes `music.afi` and `music.names.afi` for optional SH4
one-shot sample access. `--per-song library.afbm output` instead writes one
bank plus sidecars per song, useful when only one piece is resident and each
needs its own quality budget.

To create an editable initial map from MIDI program usage:

```sh
build/afx_bank --create-map library.afbm GeneralUser.sf2 \
  title_theme midi/title_theme.mid field_theme midi/field_theme.mid
```

Review source presets, coding and rates before building. AFBM sources are SF2
files; standalone PCM uses `afx_compile` or its zone-map mode.

Map options control sample rate, loop trimming, envelope, filter, gain, pan,
LFO, DSP send and SF2 modulators. Defaults use SF2 envelope/filter, standard
gain, source pan and modulators, with source reverb ignored. Supported linear
modulators are sampled at NOTE-on; use AFP lanes for sustained changes.
See the [AFBM reference](specs/assets.md#afbm-reference) for values and defaults.

`midi_channel` is an optional source selector, not an AICA output channel.
It is useful when a score reuses a MIDI program number for separate parts,
such as melodic strings and percussion. A channel-specific map wins over the
generic rule for the same song and program; a song-specific generic rule wins
over `*`.

The optional `humanize <song> <seed> <level-centibels>
<shorten-ms> <lfo-rate-steps>` may follow its `song` line in the AFBM. It is a
deterministic offline lowering step: it modifies NOTE level and KEYOFF timing,
but adds no SH4 or ARM7 feature. `lfo-rate-steps` must be `0`; rate variation
is not implemented. Put deliberate rhythmic edits in the MIDI source.

## Performance profiles

`build/afx_profile init` creates an editable JSON `.afp` file from an
AFX and binds it to that file with SHA-256. It creates defaults and empty
template/override/lane collections. `inventory` lists
`{tick, ordinal, channel}` selectors and their source setups when needed.

```sh
build/afx_profile init song.afx song.afp room 112
build/afx_profile inventory song.afx
# edit song.afp offline
build/afx_profile apply song.afx song.afc song.afp song-performance.afx song-performance.afc
build/afx_compile --visual song-performance.afx song-performance.afv
build/afx_profile describe song.afx song.afp
```

Use `defaults` for all notes, `setup_templates` for all uses of one source
setup, and `overrides` only for selected exceptions. Resolution is base setup,
defaults, setup template, note template, then note parameters. A lane at offset
zero folds into NOTE; a positive offset creates PATCH on that running voice.
The same raw AICA word parameters apply at all levels. There is no need to
list every note merely to enable DSP. See the
[AFP reference](specs/assets.md#afp--performance-profile) for the complete
schema, selectors, precedence and validation.

Applying register changes rewrites the AFX and creates a matching AFC with a
tick-zero checkpoint. SH4 reconstructs later seek positions by replaying events.
With no register changes, AFX/AFC are copied byte-for-byte. AFB and AFI are
unchanged. Regenerate AFV from the derived AFX to update the visualization.

`describe` prints `preset default-send tempo_q8_8` for the build to place in
its player metadata. Presets are `dry`, `room`, `room_warm` and `room_large`.
The SH4 player installs the DSP scene and calls `afx_instance_tempo()` using
that metadata. The derived AFX carries channel DSP-send words.
`tempo_q8_8=256` is authored speed; scaling it preserves the file's individual
NOTE/KEYOFF positions. AICA has one scene DSP program shared by all voices.

If the base AFX changes, inspect its new inventory and recreate the profile.
`apply` requires both an AFX and a matching AFC. Controlled SFX has no AFC and
cannot use this command.

The optional fourth `song` field reserves a release tail in milliseconds. It
keeps that AICA channel unavailable after its musical `KEYOFF`, so an SF2
release can fade naturally while a following note is scheduled onto another
channel. It is entirely offline: the emitted AFX still contains only ordinary
`NOTE` and `KEYOFF` commands.

## Direct N64 music and SFX

```sh
build/afx_n64 audio_control.bin audio_table.bin sequences.bin 7 song.afx
# Select another ALBank entry when the control file contains several:
build/afx_n64 audio_control.bin audio_table.bin sequences.bin 7 song.afx 1
build/afx_n64 --sfx sfx_control.bin sfx_table.bin 563 collect_item.afx
```

The music mode reads B1 ALBank + S1 CSeq directly, never through MIDI. It
lowers key/velocity splits, source envelopes, tuning, pan, VADPCM/RAW16 and
supported live CC7/CC10/CC91/pitch-bend changes to ordinary NOTE/PATCH/KEYOFF.
An infinite source loop is cut at its first boundary to produce a finite
audition flow. The optional last argument is a zero-based ALBank index.

SFX IDs are one-based raw ALInstrument roots, **not game `SOUND_*` enums**.
SFX chain interpretation follows DKR's ALBank/key-map policy.
Finite components receive KEYOFF; sustained source components use controlled
PARK so SH4 owns STOP. Finite components still receive KEYOFF in a parked
chain. Negative source decay disables amplitude decay. Each component's setup
keeps its NOTE pitch and mix as the base for live SH4 scaling.
Sample looping alone does not decide lifetime. SFX
emit AFB/AFX, not song seek/visual sidecars. See
[SFX bank maps](specs/afsfx.md) for grouping and the logical/raw-ID distinction.

To share an already-authored bank across music or SFX flows:

```sh
mkdir -p controls
build/afx_bank --merge music.afb controls raw/sequence_*.afx
```

Each input AFX needs its sibling AFB; an optional sibling AFC is relocated
along with it. Merging preserves sample bytes, coding and playback parameters,
deduplicates equal samples and rewrites bindings. It does not re-encode for
`auto`, merge AFV playlists or emit AFI. Several outputs can then use one
resident music bank; scene-specific SFX banks use the same one-bank-per-flow
contract.

## MultiPCM VGM/VGZ

```sh
build/afx_vgm soundtrack.vgz track.afx
build/afx_vgm soundtrack.vgz track.afx --gain-db -6.4 --adpcm
```

This tool lowers Sega MultiPCM registers and source sample ROM data to the
same event pipeline and emits AFB/AFX/AFC/AFV. PCM8 is the default; `--adpcm`
explicitly selects encoding. Default gain is -6.4 dB. It is not a general VGM
chip emulator: YM2612, PSG and arbitrary chips are not supported by this tool.

## Known boundaries

- MIDI timing, running status, bank/program, note-off, sustain and all-notes-off
  are parsed offline. Controllers/pressure are captured at NOTE-on for SF2
  lowering; subsequent controller changes are not emitted as PATCH.
  Use AFP lanes for sustained changes.
- SF2 supports static envelope/filter/pan/gain and supported linear modulator
  inputs sampled at NOTE-on. Curved/time-varying graphs are not a generic
  runtime modulator engine. `auto` is a deterministic signal-quality heuristic,
  not a guarantee that no human can hear the encoding difference.
- N64 CSeq and OoT AudioSeq use different parsers. AudioSeq has only an
  experimental reader. Other N64 SFX chain conventions need source-specific
  lowering; B1 alone does not guarantee DKR-compatible SFX.
- Memory and per-tick voice/register-work limits are enforced during authoring
  and runtime. A valid standalone asset can still fail to fit with other live
  banks or DSP reservations. See [Memory](memory.md) and [Testing](testing.md).

The public validator and shared emitter enforce the output contract described
in the [format reference](specs/assets.md).
