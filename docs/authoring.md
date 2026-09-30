# Authoring

The current offline compiler converts a MIDI timeline and mapping JSON into one
AFB and one bank-bound AFX. It may also emit an optional AFC seek sidecar.

```sh
python3 tools/afx_compile.py song.mid mapping.json song.afx --bank song.afb
```

Mappings select source samples, AICA envelopes, filters, LFO and DSP-send
values. A `.afp` performance profile is an offline-only description of
register-level changes: it produces a new AFX but never changes NOTE or KEYOFF
timing. Use the music source for timing and use `.afp` for timbre and
articulation.

The generic compiler currently remains Python tooling. `make compiler`
also builds the first C encoder: it reads the note and tempo subset of a
Standard MIDI file and emits a strict AFB/AFX pair with a built-in sine source.
For example:

```sh
make compiler
build/afx_compile_c song.mid song.afb song.afx
```

The initial C reader intentionally accepts only the timing and note subset
(including running status, tempo, note-off and all-notes-off). It deliberately
shares the public wire validator with the driver. Instrument mapping and
sampled-bank authoring will be added on top of this C encoder rather than
creating a second file format or runtime path.
