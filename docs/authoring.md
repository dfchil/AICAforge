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

The generic compiler currently remains Python tooling. Its inputs and output
format are stable; a deterministic C compiler is the next planned authoring
implementation.
