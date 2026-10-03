# AFSFX — offline SFX bank maps

An `.afsfx` is a tracked text description of **which source sound effects
belong together in preloaded banks**. It is useful when a game has more sounds
than fit in AICA RAM at once: keep common gameplay sounds resident, prepare
scene-specific groups when useful, and load the rest on demand.

This role is reusable across N64 projects. It is not another AICA playback
format and must not encode a new runtime sound interpreter. The resulting
assets are the same AFB sample banks and AFX register flows used for music.

## Implementation status and ownership

The implemented reader is currently **DKR's application script**,
`dreamcast/build_aicaflow_sfx.py`. AICAflow supplies the native C converter
`afx_n64 --sfx` and the lossless packer `afx_bank --merge`; neither reads an
AFSFX file itself. SH4 and ARM7 do not load this text file at runtime.

This document records that reader's real grammar. In particular, its `core`
and `vehicle` bank names and vehicle masks are DKR policy, not universal AICA
or N64 requirements. Another N64 project can reuse the grouping/residency
concept, but cannot assume its raw sound IDs, scene IDs or sequence format
match DKR. A generic native AFSFX reader is not implemented yet. Do not invent
extra directives and expect the current builder to accept them.

The checked-in DKR source map is `dreamcast/aicaflow_tools/dkr.afsfx`. Its
reader and game loader are the authority for DKR behavior; this specification
is maintained in AICAflow so other integrations can share the explanation.

## Distinguish the files

| File | Question it answers |
| --- | --- |
| AFSFX | Which complete source SFX chains should be grouped/preloaded? |
| AFBM | Which MIDI/SF2 sources, presets and conversion policies build a bank? |
| AFP | Which authored AICA register/performance values should change? |
| AFB | Where are the final encoded sample bytes? |
| AFX | Which registers should be written, and when? |
| AFI | How can SH4 locate/describe samples for direct one-shot code? |

AFSFX does not choose codec/rate, replace samples, impose sound duration, add
DSP or correct looping by sound name. Those belong to the source importer,
bank authoring and ordinary AICA commands. Listing a sound in `core` makes it
eligible for preloading; it does not force the sound to play or loop.

## Source IDs are not sample offsets or game enums

DKR's `sound-id` is a **one-based entry in the ALInstrument sound list** used
by its SFX bank. The native importer starts at that entry and follows its
source-linked component chain. A chain can use several samples and AICA
channels. List the root ID once; its component IDs need not also be listed.

Do not put these other values in an AFSFX sound list:

- A `SOUND_*` game enum / logical sound-table index. DKR first resolves that
  through `gSoundTable[logical_id].soundBite` in `src/audio.c`.
- A sample's byte address, AFI catalog position or AFB offset.
- An AICA physical channel, or a CSeq song number.

The AFX filename keeps the raw root ID (`563.afx`, for example). Several
logical game sounds may select one raw root, then apply different live pitch,
volume or pan. Correct grouping cannot compensate for a wrong ID translation.

Scene IDs are a different space: zero-based application scene/level IDs.
The loader receives one of these IDs when preparing a scene.

## Current DKR text grammar

The file is whitespace-separated text, read as UTF-8 on the supported hosts.
Blank lines and `#` comments are ignored. An inline `#` ends a line. There are
no quoted paths, JSON objects, binary headers, version/magic directive, sample
names or references to another map. Directive names are case-sensitive.

```text
scene-count <N>
vehicle-masks <mask-for-scene-0> ... <mask-for-scene-N-minus-1>
bank core <sound-id> [sound-id ...]
bank vehicle <sound-id> [sound-id ...]
scene <scene-id> <sound-id> [sound-id ...]
```

Write IDs and counts as decimal integers. Use one `scene-count` and one
`vehicle-masks` declaration, both required. Declare both banks even if a
particular scene needs no vehicle sounds; the current grammar requires each
bank to have at least one ID and accepts exactly those two bank names.
Optional `scene` lines may appear in any order. A missing scene line means
"no scene-local preload pack", not "no sounds may play in this scene".

| Directive | Meaning and validation |
| --- | --- |
| `scene-count N` | `N >= 1`; declares the range `0..N-1`. |
| `vehicle-masks ...` | Exactly N values, each `0..7`; car=1, hovercraft=2, plane=4, combinations use bitwise OR. |
| `bank core ...` | Common resident raw sound IDs; IDs must be positive. |
| `bank vehicle ...` | Vehicle-class raw sound IDs; IDs must be positive. |
| `scene id ...` | One nonempty raw-ID list for scene `0..N-1`. |

The reader sorts and deduplicates IDs within each list. Duplicate `bank` or
`scene` definitions, missing required fields, unknown directives, invalid
integers, out-of-range scenes/masks, and overlap between core and vehicle IDs
are errors. Source existence/upper sound-ID bounds are checked by the native
importer, not inferred from the map. Avoid repeating a resident sound in a
scene pack; the current reader does not reject that redundant cross-pack copy.
Repeated `scene-count`/`vehicle-masks` declarations currently replace the
previous value; authors should not rely on that parser behavior.

### Small example

This is a three-scene illustration using DKR raw sound IDs, not a replacement
for the full game map:

```text
scene-count 3
vehicle-masks 1 0 4

bank core 1 4 563       # common sounds, including a complete component chain
bank vehicle 42 43

scene 0 92
scene 2 89 91
```

Scene 0 has a local pack and needs vehicle audio. Scene 1 has neither a local
pack nor a vehicle requirement; it may still use core and fallback sounds.
Scene 2 has another local pack and needs vehicle audio.

**Current loader detail:** any nonzero vehicle mask loads the entire single
`vehicle.afb`. It does not load separate car/plane/hovercraft banks or filter
that bank's samples by bit. The bits preserve the source scene policy, while
the current residency decision is simply zero versus nonzero.

## Build path and generated output

In a DKR checkout, after extraction and `make -C third_party/aicaflow compiler`:

```sh
python3 dreamcast/build_aicaflow_sfx.py . \
  third_party/aicaflow/build/afx_n64 \
  third_party/aicaflow/build/afx_bank \
  dreamcast/aicaflow_tools/dkr.afsfx build/dc/aicaflow
```

Use the same Python 3.10+ interpreter selected for the game build. `Makefile.dc`
already runs this command when its source inputs, map or native tools change;
manual generation is for inspection. The application script reads extracted
SFX control/table inputs `asset_audio_2.bin` and `asset_audio_3.bin`.

For each pack, it invokes the C importer for each raw root, then invokes the C
merger. Samples are deduplicated **within** a final bank; the merger preserves
their codec and encoded bytes. Cross-bank duplicates remain separate because
one AFX binds to one AFB, not a collection of banks.

```text
build/dc/aicaflow/
  core.afb                 common sample block
  core/<raw-id>.afx        flows bound to core.afb
  vehicle.afb              one vehicle sample block
  vehicle/<raw-id>.afx     flows bound to vehicle.afb
  scenes/<scene-id>.afb    one block for each listed scene
  scenes/<scene-id>/<raw-id>.afx
  sfx_manifest.h           generated SH4 tables: IDs, presence, masks and sizes
  manifest.json            generated build-verification metadata
```

Hidden `.core.raw`, `.vehicle.raw` and per-scene `.raw` directories contain
intermediate per-sound AFB/AFX pairs. They are build artifacts, not source maps
or additional runtime formats. Regenerate outputs; do not hand-edit manifests,
bank identities or relocated sample addresses.

Fallback is built separately by `dreamcast/build_aicaflow_fallback.py` for
**every** source root, including ones absent from the map. It produces
`fallback/<raw-id>.afb`, `fallback/controls/<raw-id>.afx` and its own manifest.
AFSFX changes grouping, not the fallback corpus.

## Runtime residency and lifetime

The generated header, not the text map, is compiled into DKR. The game keeps
the core bank resident. It keeps the vehicle bank when required and attempts
to preload a scene-local bank when memory permits; otherwise it falls back to
individual on-demand pairs. A declared scene pack is not a guarantee that it
can coexist with every music flow, DSP ring and active voice.

The loader's current 64 KiB headroom test is a preload decision, not a reserved
allocator partition. Its calculated scene size includes aligned sample-bank
payload and AFX images, not just the `.afb` file size. Active flows retain
their bank; scene teardown stops/recycles instances before releasing it.

Sample looping and sound lifetime are independent. The C importer derives
KEYOFF/END or controlled PARK from the source chain/envelope semantics. A
sample loop does not by itself mean a forever-playing SFX. SH4 owns STOP for a
parked flow. AFSFX must not contain per-sound timeout hacks.

For another N64 project, establish its raw-ID/chain semantics, logical-ID
translation, scene transitions and preload policy first. A B1 ALBank does not
imply that the game's SFX-chain metadata or music sequence language is DKR's;
OoT AudioSeq is a separate source format. Reuse AFB/AFX and the shared authoring
pipeline without claiming that the existing DKR application parser is universal.

## Verification and safe editing

Add `--verify` to the pack command to compare existing outputs with the map:

```sh
python3 dreamcast/build_aicaflow_sfx.py . \
  third_party/aicaflow/build/afx_n64 \
  third_party/aicaflow/build/afx_bank \
  dreamcast/aicaflow_tools/dkr.afsfx build/dc/aicaflow --verify
make -f Makefile.dc aicaflow-fallback-verify
```

The first check validates membership, masks, scene sizes and basic AFB/AFX
file ranges. It does not recompile/recompare source audio or prove that an ID
is the sound a human intended. Fallback verification checks recorded file
lengths and SHA-256 values. Source/register parity is a separate check;
see [Authoring parity](../authoring-parity.md).

When editing a map, resolve raw IDs from the source, regenerate with the
application build, inspect pack memory costs and test the relevant scene and
live controls. Commit the map and build/documentation changes, not extracted
game samples. Residency may improve latency; it does not add more than AICA's
64 hardware voices or bypass runtime execution/memory limits.
