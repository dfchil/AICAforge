#!/usr/bin/env python3
"""AFBM song structure and trimming: `section`, `order` and `trim` lines through afx_bank."""
import math
import os
import struct
import subprocess
import tempfile
from pathlib import Path

BUILD = Path(os.environ.get("AFX_BUILD_DIR", Path(__file__).resolve().parents[1] / "build"))
SAMPLE_ID, INSTRUMENT = 53, 41


def chunk(tag, data):
    return tag + struct.pack("<I", len(data)) + data + (b"\0" if len(data) & 1 else b"")


def font():
    """One preset playing one 20000-frame one-shot sine: much longer than the notes below hold it."""
    frames = 20000
    pcm = b"".join(struct.pack("<h", int(12000 * math.sin(i * 2 * math.pi / 100))) for i in range(frames))
    name = lambda text: text.encode().ljust(20, b"\0")
    igen = struct.pack("<Hh", SAMPLE_ID, 0) + struct.pack("<Hh", 0, 0)
    pdta = (chunk(b"phdr", name("preset") + struct.pack("<HHHIII", 0, 0, 0, 0, 0, 0) +
                  name("EOP") + struct.pack("<HHHIII", 0, 0, 1, 0, 0, 0)) +
            chunk(b"pbag", struct.pack("<HHHH", 0, 0, 1, 0)) + chunk(b"pmod", bytes(10)) +
            chunk(b"pgen", struct.pack("<HhHh", INSTRUMENT, 0, 0, 0)) +
            chunk(b"inst", name("instrument") + struct.pack("<H", 0) + name("EOI") + struct.pack("<H", 1)) +
            chunk(b"ibag", struct.pack("<HHHH", 0, 0, 1, 0)) + chunk(b"imod", bytes(10)) + chunk(b"igen", igen) +
            chunk(b"shdr", name("tone") + struct.pack("<IIIIIBbHH", 0, frames, 8, frames - 8, 44100, 60, 0, 0, 1) +
                  name("EOS") + bytes(26)))
    body = (b"sfbk" + chunk(b"LIST", b"INFO" + chunk(b"ifil", struct.pack("<HH", 2, 1)) +
                                  chunk(b"isng", b"EMU8000\0") + chunk(b"INAM", b"test\0\0")) +
            chunk(b"LIST", b"sdta" + chunk(b"smpl", pcm + bytes(46 * 2))) + chunk(b"LIST", b"pdta" + pdta))
    return b"RIFF" + struct.pack("<I", len(body)) + body


def midi(starts_ms, hold_ms=200):
    """Notes (key 60) at the given milliseconds (480 ticks a quarter at 120 bpm: 0.96 ticks a millisecond)."""
    def varlen(value):
        out = [value & 0x7F]
        while value > 0x7F:
            value >>= 7
            out.insert(0, (value & 0x7F) | 0x80)
        return bytes(out)
    events, now = b"", 0
    for start in starts_ms:
        on, off = round(start * 0.96), round((start + hold_ms) * 0.96)
        events += varlen(on - now) + bytes([0x90, 60, 100]) + varlen(off - on) + bytes([0x80, 60, 0])
        now = off
    events += b"\x00\xFF\x2F\x00"
    return b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + b"MTrk" + struct.pack(">I", len(events)) + events


with tempfile.TemporaryDirectory(prefix="afx-bank-map-") as tmp:
    d = Path(tmp)
    (d / "font.sf2").write_bytes(font())
    # Intro, A (1000..3000), A again, B (5000..6000).
    (d / "tune.mid").write_bytes(midi([0, 1000, 2000, 3000, 4000, 5000]))
    structure = ("section tune intro 0 1000\nsection tune a 1000 3000\nsection tune b 5000 6000\n"
                 "order tune intro a a b loop=a\n")
    head = "source font font.sf2\nmap * 0 0 font 0 0 pcm8\n"
    songs = "song tune tune.mid\n" + structure + "song plain tune.mid\n"
    (d / "whole.afbm").write_text(head + songs)
    (d / "trim.afbm").write_text("trim\n" + head + songs)
    for label in ("whole", "trim"):
        out = d / label
        out.mkdir()
        subprocess.run([str(BUILD / "afx_bank"), "--per-song", str(d / f"{label}.afbm"), str(out)], check=True)
        afm = (out / "tune.afm").read_bytes()
        assert afm[:4] == b"AFM1" and not (out / "tune.afx").exists()
        image = struct.unpack_from("<I", afm, 16)[0]
        assert struct.unpack_from("<3I", afm, image) == (3, 4, 1)
        assert (out / "plain.afx").read_bytes()[:4] == b"AFX2"
        # One shared bank for both: the song and the flow bind the same identity.
        shared = d / f"{label}-shared"
        shared.mkdir()
        subprocess.run([str(BUILD / "afx_bank"), str(d / f"{label}.afbm"), str(shared), str(shared / "bank.afb")],
                       check=True)
        identity = struct.unpack_from("<2I", (shared / "bank.afb").read_bytes(), 8)
        assert struct.unpack_from("<2I", (shared / "tune.afm").read_bytes(), 36) == identity
        assert struct.unpack_from("<2I", (shared / "plain.afx").read_bytes(), 40) == identity
    whole, trimmed = (d / "whole/tune.afb").stat().st_size, (d / "trim/tune.afb").stat().st_size
    assert trimmed < whole // 2, (trimmed, whole)  # notes held 200 ms read under half of the 20000 frames
    assert (d / "trim-shared/bank.afb").stat().st_size < (d / "whole-shared/bank.afb").stat().st_size
    # A section without an order, or a section for a song the map does not name, is refused.
    (d / "bad.afbm").write_text(head + "song tune tune.mid\nsection tune a 0 100\n")
    assert subprocess.run([str(BUILD / "afx_bank"), "--per-song", str(d / "bad.afbm"), str(d)],
                          capture_output=True).returncode != 0
    (d / "bad2.afbm").write_text(head + "song tune tune.mid\nsection other a 0 100\n")
    assert subprocess.run([str(BUILD / "afx_bank"), "--per-song", str(d / "bad2.afbm"), str(d)],
                          capture_output=True).returncode != 0

print("AFBM sections, order and trim through afx_bank: pass")
