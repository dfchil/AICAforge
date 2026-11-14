#!/usr/bin/env python3
"""Small self-contained MultiPCM VGM smoke test for the native importer."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile


TOOL = Path(os.environ.get("AFX_BUILD_DIR", Path(__file__).resolve().parents[1] / "build")) / "afx_vgm"


def write_register(data: bytearray, address: int, value: int) -> None:
    data.extend((0xB5, 2, address, 0xB5, 0, value))


def wait_ms(data: bytearray, ms: int) -> None:
    samples = ms * 441 // 10
    while samples:
        step = min(samples, 0xFFFF)
        data += bytes((0x61,)) + struct.pack("<H", step)
        samples -= step


def song_fixture() -> bytes:
    """Intro, A, A, B, then the VGM loop jumps back into A: B's last 100 ms repeat A's opening."""
    header = bytearray(0x8C)
    header[:4] = b"Vgm "
    struct.pack_into("<I", header, 8, 0x161)
    struct.pack_into("<I", header, 0x34, 0x58)
    struct.pack_into("<I", header, 0x88, 8_000_000)
    rom = bytearray(44)
    rom[5:7] = b"\xff\xe0"
    rom[8:11] = b"\xf0\x00\x0f"  # fast attack, held at full level, fast release: tails stay short
    rom[12:] = bytes(range(32))
    data = bytearray((0x67, 0x66, 0x89))
    data += struct.pack("<I", len(rom) + 8) + struct.pack("<II", len(rom), 0) + rom
    events = []  # (ms, slot, key on, pitch high byte)
    events += [(0, 0, 1, 0x10), (100, 0, 0, 0x10)]  # intro
    for start in (500, 1500):  # A twice
        events += [(start, 1, 1, 0x20), (start + 200, 1, 0, 0x20), (start + 400, 2, 1, 0x30), (start + 600, 2, 0, 0x30)]
    events += [(2500, 3, 1, 0x40), (2700, 3, 0, 0x40)]  # B
    events += [(3000, 1, 1, 0x20), (3050, 1, 0, 0x20)]  # B's ending repeats A's opening (to the loop point at 600 ms)
    now = 0
    for at, slot, on, pitch in sorted(events):
        wait_ms(data, at - now); now = at
        data += bytes((0xB5, 1, slot))
        if on:
            write_register(data, 1, 0)
            write_register(data, 2, 0)
            write_register(data, 3, pitch)
            write_register(data, 0, 0x70)
        write_register(data, 4, 0x80 if on else 0)
    wait_ms(data, 3100 - now)
    data += b"\x66"
    total, loop = 3100 * 441 // 10, (3100 - 600) * 441 // 10
    struct.pack_into("<I", header, 0x18, total)
    struct.pack_into("<I", header, 0x1C, 0x40)  # any non-zero loop offset marks a loop
    struct.pack_into("<I", header, 0x20, loop)
    return bytes(header + data)


def fixture() -> bytes:
    header = bytearray(0x8C)
    header[:4] = b"Vgm "
    struct.pack_into("<I", header, 8, 0x161)
    struct.pack_into("<I", header, 0x34, 0x58)  # data begins at 0x8c
    struct.pack_into("<I", header, 0x88, 8_000_000)  # one MultiPCM chip
    rom = bytearray(44)
    rom[5:7] = b"\xff\xe0"  # 32 decoded PCM8 frames
    rom[8:11] = b"\x11\x11\x11"
    rom[12:] = bytes(range(32))
    data = bytearray((0x67, 0x66, 0x89))
    data += struct.pack("<I", len(rom) + 8) + struct.pack("<II", len(rom), 0) + rom
    data += bytes((0xB5, 1, 0))  # select source slot zero
    write_register(data, 1, 0)   # sample ID, loads ROM defaults
    write_register(data, 2, 0)
    write_register(data, 3, 0x10)
    write_register(data, 0, 0x70)
    write_register(data, 4, 0x80)  # KEYON
    data += bytes((0x61, 0x44, 0xAC))
    write_register(data, 4, 0)     # KEYOFF
    data += b"\x66"
    return bytes(header + data)


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    source, output = root / "fixture.vgm", root / "fixture.afx"
    source.write_bytes(fixture())
    subprocess.run([str(TOOL), str(source), str(output)], check=True)
    afb, afc, afv = (output.with_suffix(suffix) for suffix in (".afb", ".afc", ".afv"))
    assert output.read_bytes()[:4] == b"AFX2"
    assert afb.read_bytes()[:4] == b"AFB\0"
    assert afc.read_bytes()[:4] == b"AFC\0"
    assert afv.read_bytes()[:4] == b"VIZ1"

    # A song: intro, A stored once and played twice, B, looping back to A.
    source, output = root / "song.vgm", root / "song.afm"
    source.write_bytes(song_fixture())
    subprocess.run([str(TOOL), str(source), str(output), "--song", "500:1000"], check=True)
    afm = output.read_bytes()
    assert afm[:4] == b"AFM1"
    image = struct.unpack_from("<I", afm, 16)[0]
    sections, orders, loop_index, table, order_at = struct.unpack_from("<5I", afm, image)
    assert (sections, orders, loop_index) == (3, 4, 1), (sections, orders, loop_index)
    assert struct.unpack_from("<4H", afm, image + order_at) == (0, 1, 1, 2)
    assert [struct.unpack_from("<3I", afm, image + table + 12 * i)[2] for i in range(3)] == [500, 1000, 500]
    assert output.with_suffix(".afb").read_bytes()[:4] == b"AFB\0"

print("native VGM importer fixture passed")
