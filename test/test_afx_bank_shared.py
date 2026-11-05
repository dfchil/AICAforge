#!/usr/bin/env python3
"""afx_bank --merge --shared: samples a shared bank holds play from its slot."""

import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("AFX_BUILD_DIR", ROOT / "build"))
FLAG_BANKS = 64
SLOT_SHIFT = 21


def run(*arguments: str, ok: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run([str(BUILD / "afx_bank"), "--merge", *arguments], capture_output=True, text=True)
    assert (result.returncode == 0) == ok, result.stderr
    return result


def bank(identity: tuple, payload: bytes) -> bytes:
    payload += bytes(-len(payload) % 32)
    return struct.pack("<8I", 0x00424641, 1, *identity, 32, len(payload), 32 + len(payload), 0) + payload


def setups(flow: bytes) -> list:
    """(slot, offset, bytes) per setup, checked against the setup's address."""
    header = struct.unpack_from("<20I", flow)
    out = []
    for i in range(header[13]):
        pair, address, size = struct.unpack_from("<3I", flow, header[12] + 12 * i)
        control, low = struct.unpack_from("<HH", flow, header[4] + pair)
        assert ((control & 0x7F) << 16 | low) == address
        out.append((address >> SLOT_SHIFT, address & ((1 << SLOT_SHIFT) - 1), size))
    return out


def main() -> None:
    with tempfile.TemporaryDirectory() as directory:
        d = Path(directory)
        subprocess.run([sys.executable, str(ROOT / "test/make_cli_fixture.py"), str(d)], check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run([str(BUILD / "afx_compile"), str(d / "fixture.mid"), "--zones", str(d / "fixture.zones"),
                        str(d / "song.afb"), str(d / "song.afx")], check=True, stdout=subprocess.DEVNULL)
        source = (d / "song.afb").read_bytes()[32:]
        original = setups((d / "song.afx").read_bytes())
        assert len({(offset, size) for _, offset, size in original}) >= 2
        # A core bank holding the first sample, 64 bytes in.
        _, first, first_size = original[0]
        first_bytes = source[first:first + first_size]
        (d / "core.afb").write_bytes(bank((0xC0DE0001, 0xC0DE0002), bytes(64) + first_bytes))

        (d / "controls").mkdir()
        run("--shared", str(d / "core.afb"), str(d / "music.afb"), str(d / "controls"), str(d / "song.afx"))
        flow = (d / "controls/song.afx").read_bytes()
        music = (d / "music.afb").read_bytes()
        header = struct.unpack_from("<20I", flow)
        assert header[3] & FLAG_BANKS and header[10:12] == struct.unpack_from("<2I", music, 8)
        assert struct.unpack_from("<3I", flow, header[15]) == (1, 0xC0DE0001, 0xC0DE0002)
        table = (header[6] + header[7] + 3) & ~3
        assert header[5] == table + 16 and flow[header[4] + table:header[4] + table + 16] == bytes(16)
        payloads = {0: music[32:], 1: bank((0, 0), bytes(64) + first_bytes)[32:]}
        assert first_bytes not in music[32:]
        for (slot, offset, size), (_, old, _) in zip(setups(flow), original):
            assert payloads[slot][offset:offset + size] == source[old:old + size]
            assert slot == (1 if source[old:old + size] == first_bytes else 0)
        # The seek states moved with their setups.
        seek = (d / "controls/song.afc").read_bytes()
        assert struct.unpack_from("<3I", seek, 8) == (header[8], header[10], header[11])
        entries, cursor = struct.unpack_from("<I", seek, 40)[0], 48
        for _ in range(entries):
            states = struct.unpack_from("<I", seek, cursor + 12)[0]
            cursor += 16
            for _ in range(states):
                control, low = struct.unpack_from("<HH", seek, cursor + 4)
                address = (control & 0x7F) << 16 | low
                assert (address >> SLOT_SHIFT, address & ((1 << SLOT_SHIFT) - 1)) in \
                    {(slot, offset) for slot, offset, _ in setups(flow)}
                cursor += 40
        assert cursor == len(seek)

        # Every sample shared: one bank, no flag, no merged bank written.
        (d / "only").mkdir()
        run("--shared", str(d / "song.afb"), str(d / "only.afb"), str(d / "only"), str(d / "song.afx"))
        only = (d / "only/song.afx").read_bytes()
        assert not (d / "only.afb").exists() and not struct.unpack_from("<I", only, 12)[0] & FLAG_BANKS
        assert struct.unpack_from("<2I", only, 40) == struct.unpack_from("<2I", (d / "song.afb").read_bytes(), 8)

        # Four banks per flow: three shared ones at most.
        shared = ["--shared", str(d / "core.afb")] * 4
        run(*shared, str(d / "x.afb"), str(d / "only"), str(d / "song.afx"), ok=False)
    print("afx_bank --merge --shared: PASS")


if __name__ == "__main__":
    main()
