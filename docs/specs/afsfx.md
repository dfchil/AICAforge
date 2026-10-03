# AFSFX — SFX bank maps

AFSFX is an offline text map that groups source sound effects into preloaded
banks. Build outputs are ordinary AFB sample banks and bank-bound AFX flows.
The application loads the banks; SH4 and ARM7 do not parse AFSFX.

The reader is DKR's `dreamcast/build_aicaflow_sfx.py`, using AICAflow's
`afx_n64 --sfx` converter and `afx_bank --merge` packer. The map is
`dreamcast/aicaflow_tools/dkr.afsfx` in the DKR repository. The grammar below
is DKR-specific, including `core`, `vehicle` and vehicle masks.

## File roles

| Input | Controls |
| --- | --- |
| AFSFX | SFX grouping and preload membership |
| AFBM | MIDI/SF2 sources and bank conversion policy |
| AFP | AICA register and performance adjustments |

Sample coding, envelopes and sound lifetime are handled by the importer.
AFI catalogs describe samples for SH4 one-shot code; they do not group SFX.

## IDs

A `sound-id` is a **one-based raw ALInstrument sound-chain root**. The importer
follows its component chain, which can use several samples and AICA channels.
List the root once; its components are included automatically.

DKR translates logical `SOUND_*` IDs through
`gSoundTable[logical_id].soundBite` in `src/audio.c`. Use the resulting raw ID
in the map, not the enum, an AFI position, a sample offset or an AICA channel.
Generated flow filenames keep the raw ID, for example `563.afx`.

Scene IDs are zero-based application level IDs. Several logical sounds can
share a raw root while applying different live pitch, volume or pan.

## Grammar

UTF-8 whitespace-separated text. Blank lines and `#` comments are ignored;
inline `#` comments are supported. Directives are case-sensitive. Numbers are
decimal integers.

```text
scene-count <N>
vehicle-masks <mask-for-scene-0> ... <mask-for-scene-N-minus-1>
bank core <sound-id> [sound-id ...]
bank vehicle <sound-id> [sound-id ...]
scene <scene-id> <sound-id> [sound-id ...]
```

| Directive | Requirements |
| --- | --- |
| `scene-count N` | Required; `N >= 1`. |
| `vehicle-masks ...` | Required; exactly N values in `0..7`. Car=1, hovercraft=2, plane=4; combine with bitwise OR. |
| `bank core ...` | Required, nonempty positive raw-ID list. |
| `bank vehicle ...` | Required, nonempty positive raw-ID list. |
| `scene id ...` | Optional, nonempty raw-ID list for scene `0..N-1`. |

Only the two named banks are supported. IDs in each list are sorted and
deduplicated. Duplicate bank/scene declarations, unknown directives, missing
fields, invalid integers, out-of-range masks/scenes and overlapping core/vehicle
IDs are errors. The importer checks whether each raw ID exists in the source.

Declare `scene-count` and `vehicle-masks` once: repeated declarations replace
the earlier values. Avoid listing a resident sound in a scene pack; this
redundancy is allowed by the parser but duplicates sample data across banks.

### Example

```text
scene-count 3
vehicle-masks 1 0 4

bank core 1 4 563       # common sounds
bank vehicle 42 43

scene 0 92
scene 2 89 91
```

Scenes 0 and 2 have local packs and need vehicle audio. Scene 1 uses core and
on-demand sounds. A missing scene line means no local preload pack.
Any nonzero vehicle mask loads the **entire** `vehicle.afb`; masks do not select
separate car, plane or hovercraft sample subsets.

## Build

Run from an extracted DKR checkout with Python 3.10+:

```sh
make -C third_party/aicaflow compiler
python3 dreamcast/build_aicaflow_sfx.py . \
  third_party/aicaflow/build/afx_n64 \
  third_party/aicaflow/build/afx_bank \
  dreamcast/aicaflow_tools/dkr.afsfx build/dc/aicaflow
```

`Makefile.dc` invokes the builder when inputs change. Source SFX control and
sample data are `asset_audio_2.bin` and `asset_audio_3.bin`.
The builder converts each raw root and merges each pack, deduplicating encoded
samples within the bank without re-encoding them.

```text
build/dc/aicaflow/
  core.afb                 common sample block
  core/<raw-id>.afx        controls bound to core.afb
  vehicle.afb              vehicle sample block
  vehicle/<raw-id>.afx     controls bound to vehicle.afb
  scenes/<scene-id>.afb    local sample blocks
  scenes/<scene-id>/<raw-id>.afx
  sfx_manifest.h           SH4 IDs, masks, presence and memory sizes
  manifest.json            verification metadata
```

Hidden `.core.raw`, `.vehicle.raw` and per-scene `.raw` directories hold
intermediate AFB/AFX pairs. Edit the map and regenerate; manifests and bank
bindings are generated data.

`dreamcast/build_aicaflow_fallback.py` separately builds every source root as
`fallback/<raw-id>.afb` and `fallback/controls/<raw-id>.afx`. Removing a sound
from a preload pack leaves it available on demand.

## Residency and lifetime

DKR compiles the generated header into the game, keeps core resident, loads
vehicle when needed and preloads scene banks when memory permits. Otherwise,
requests use on-demand pairs.

Scene preload checks leave 64 KiB headroom. This is a loading threshold, not
an allocator reservation. Pack costs include aligned AFB payloads and AFX
images; music, DSP and other live allocations also consume the arena.

A flow retains its bank. Scene teardown must stop/recycle instances and free
flows before releasing the bank. The importer derives KEYOFF/END or PARK from
source chain/envelope semantics. Looped samples can belong to finite sounds;
SH4 owns STOP for parked flows.

For another N64 project, define its logical-to-raw ID translation, chain
semantics and scene policy. B1 ALBank alone does not imply DKR-compatible SFX.
OoT AudioSeq is a separate sequence language.

## Verify

```sh
python3 dreamcast/build_aicaflow_sfx.py . \
  third_party/aicaflow/build/afx_n64 \
  third_party/aicaflow/build/afx_bank \
  dreamcast/aicaflow_tools/dkr.afsfx build/dc/aicaflow --verify
make -f Makefile.dc aicaflow-fallback-verify
```

Pack verification checks membership, masks, scene sizes and basic AFB/AFX
ranges. Fallback verification checks recorded file lengths and SHA-256 values.
Test sound selection, live controls, lifetime and scene transitions in the game;
see [Testing](../testing.md).
