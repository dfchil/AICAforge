# Research tools

Experimental utilities for SoundFont inspection, codec checks and sequence
tracing. They are not used by example asset builds. Supported authoring tools
are in `../author/`.

`afx_n64.py` is the one N64 reader entry point:

```sh
# libaudio CSeq + B1 ALBank: emit a complete bank-bound flow.
python3 tools/research/afx_n64.py cseq control.bin samples.tbl sequences.bin 7 song.afx

# OoT AudioSeq: lower its sequence/channel/layer program to a trace.
python3 tools/research/afx_n64.py audioseq /path/to/oot-dc 7 song.trace.json
```

The formats have independent parsers, but both produce the same AICA register
semantics: notes, KEYOFFs, pan, pitch and DSP sends. AudioSeq traces retain
source sample IDs; an OoT bank build resolves those IDs from its extracted
`Audiobank`/`Audiotable` and packs the AFB. This keeps N64 ROM extraction out
of the generic AFB format and runtime.
