#!/usr/bin/env python3
"""The C SoundFont reader against small synthetic fonts.

Run by `make check`; needs only the standard library and build/afx_compile.
"""
import math
import os
import struct
import subprocess
import tempfile
from pathlib import Path

BUILD = Path(os.environ.get("AFX_BUILD_DIR", Path(__file__).resolve().parents[1] / "build"))
SAMPLE_MODES, SAMPLE_ID, COARSE_TUNE, INSTRUMENT = 54, 53, 51, 41


def chunk(tag, data):
    return tag + struct.pack("<I", len(data)) + data + (b"\0" if len(data) & 1 else b"")


def font(global_zone=(), local_zone=()):
    """One preset, one instrument, one 2000-frame sample with header loop points."""
    frames = 2000
    pcm = b"".join(struct.pack("<h", int(12000 * math.sin(i * 2 * math.pi / 100))) for i in range(frames))
    name = lambda text: text.encode().ljust(20, b"\0")
    zones = ([list(global_zone)] if global_zone else []) + [list(local_zone) + [(SAMPLE_ID, 0)]]
    ibag, igen = b"", b""
    for zone in zones:
        ibag += struct.pack("<HH", len(igen) // 4, 0)
        igen += b"".join(struct.pack("<Hh", oper, amount) for oper, amount in zone)
    ibag += struct.pack("<HH", len(igen) // 4, 0)
    igen += struct.pack("<Hh", 0, 0)
    pdta = (chunk(b"phdr", name("preset") + struct.pack("<HHHIII", 0, 0, 0, 0, 0, 0) +
                  name("EOP") + struct.pack("<HHHIII", 0, 0, 1, 0, 0, 0)) +
            chunk(b"pbag", struct.pack("<HHHH", 0, 0, 1, 0)) +
            chunk(b"pmod", bytes(10)) +
            chunk(b"pgen", struct.pack("<HhHh", INSTRUMENT, 0, 0, 0)) +
            chunk(b"inst", name("instrument") + struct.pack("<H", 0) + name("EOI") + struct.pack("<H", len(zones))) +
            chunk(b"ibag", ibag) + chunk(b"imod", bytes(10)) + chunk(b"igen", igen) +
            chunk(b"shdr", name("tone") + struct.pack("<IIIIIBbHH", 0, frames, 8, 1992, 44100, 60, 0, 0, 1) +
                  name("EOS") + bytes(26)))
    body = (b"sfbk" + chunk(b"LIST", b"INFO" + chunk(b"ifil", struct.pack("<HH", 2, 1)) +
                                  chunk(b"isng", b"EMU8000\0") + chunk(b"INAM", b"test\0\0")) +
            chunk(b"LIST", b"sdta" + chunk(b"smpl", pcm + bytes(46 * 2))) +
            chunk(b"LIST", b"pdta" + pdta))
    return b"RIFF" + struct.pack("<I", len(body)) + body


MIDI = (b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + b"MTrk" + struct.pack(">I", 13) +
        bytes([0, 0x90, 60, 100, 0x83, 0x60, 0x80, 60, 0, 0, 0xFF, 0x2F, 0]))


def compile_font(directory, label, data):
    (directory / f"{label}.sf2").write_bytes(data)
    (directory / "note.mid").write_bytes(MIDI)
    subprocess.run([str(BUILD / "afx_compile"), str(directory / "note.mid"), "--sf2", "auto",
                    str(directory / f"{label}.sf2"), str(directory / f"{label}.afb"),
                    str(directory / f"{label}.afx")], check=True, stdout=subprocess.DEVNULL)
    # Skip each file's identity words: compare what the font put into it.
    return (directory / f"{label}.afb").read_bytes()[32:], (directory / f"{label}.afx").read_bytes()[96:]


with tempfile.TemporaryDirectory(prefix="afx-sf2-c-") as tmp:
    tmp = Path(tmp)
    unset = compile_font(tmp, "unset", font())
    one_shot = compile_font(tmp, "oneshot", font(local_zone=[(SAMPLE_MODES, 0)]))
    looped = compile_font(tmp, "looped", font(local_zone=[(SAMPLE_MODES, 1)]))
    # sampleModes defaults to 0: header loop points alone do not make a loop.
    assert unset == one_shot, "a zone without sampleModes must be a one-shot"
    assert looped != one_shot, "sampleModes 1 must loop"
    inherited = compile_font(tmp, "inherited", font(global_zone=[(SAMPLE_MODES, 1)]))
    assert inherited == looped, "a global zone's sampleModes is the local zone's default"

    local = compile_font(tmp, "local", font(local_zone=[(COARSE_TUNE, 2)]))
    both = compile_font(tmp, "both", font(global_zone=[(COARSE_TUNE, 1)], local_zone=[(COARSE_TUNE, 2)]))
    only_global = compile_font(tmp, "global", font(global_zone=[(COARSE_TUNE, 1)]))
    # A local generator replaces the global zone's; it does not add to it.
    assert both == local, "local coarseTune must supersede the global zone's"
    assert only_global != local and only_global != unset, "a global coarseTune still applies"

print("C SoundFont zone default and precedence checks passed")
