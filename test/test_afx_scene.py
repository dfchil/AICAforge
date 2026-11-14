#!/usr/bin/env python3
"""afx_scene: AFS files planned from a manifest, and the fit check."""

import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("AFX_BUILD_DIR", ROOT / "build"))
FIRMWARE = ROOT / "dependencies/AICAflow/firmware/aicaflow.drv"
RING = 0x4000


def scene(manifest: Path, ok: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run([str(BUILD / "afx_scene"), str(manifest)], capture_output=True, text=True)
    assert (result.returncode == 0) == ok, result.stdout + result.stderr
    return result


def entries(afs: bytes) -> list:
    """(kind, name, file bytes, block offset, block bytes, ticks) per entry."""
    magic, version, _, _, total, block, count, reserved = struct.unpack_from("<8I", afs)
    assert (magic, version, total, reserved) == (0x00534641, 1, len(afs), 0) and block % 32 == 0
    out = []
    for i in range(count):
        kind, name, offset, size, at, room, ticks, _ = struct.unpack_from("<8I", afs, 32 + 32 * i)
        label = afs[name:afs.index(b"\0", name)].decode()
        out.append((kind, label, afs[offset:offset + size], at, room, ticks))
        assert at + room <= block and offset % 32 == 0
    return out


def main() -> None:
    with tempfile.TemporaryDirectory() as directory:
        d = Path(directory)
        subprocess.run([sys.executable, str(ROOT / "test/make_cli_fixture.py"), str(d)], check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run([str(BUILD / "afx_compile"), str(d / "fixture.mid"), "--zones", str(d / "fixture.zones"),
                        str(d / "song.afb"), str(d / "song.afx")], check=True, stdout=subprocess.DEVNULL)
        # A core bank with the first sample, and a level bank with the rest: the
        # level's flow plays from both.
        song = (d / "song.afx").read_bytes()
        header = struct.unpack_from("<20I", song)
        pair, address, size = struct.unpack_from("<3I", song, header[12])
        payload = (d / "song.afb").read_bytes()[32:][address:address + size]
        payload += bytes(-len(payload) % 32)
        core = struct.pack("<8I", 0x00424641, 1, 0xC0DE0001, 0xC0DE0002, 32, len(payload),
                           32 + len(payload), 0) + payload
        (d / "core.afb").write_bytes(core)
        (d / "controls").mkdir()
        subprocess.run([str(BUILD / "afx_bank"), "--merge", "--shared", str(d / "core.afb"), str(d / "music.afb"),
                        str(d / "controls"), str(d / "song.afx")], check=True, stdout=subprocess.DEVNULL)
        flow = (d / "controls/song.afx").read_bytes()
        assert struct.unpack_from("<I", flow, 12)[0] & 64

        permanent = f"firmware {FIRMWARE}\npermanent core out/core.afs\n  bank core core.afb\n  ring 0\n"
        level = "level forest out/forest.afs  # a level\n  bank music music.afb\n  flow song controls/song.afx\n" \
                "  slot extra 100\n"
        (d / "out").mkdir()
        (d / "scenes.afsm").write_text(permanent + level)
        report = scene(d / "scenes.afsm").stdout
        base = (FIRMWARE.stat().st_size + 31) & ~31
        assert f"arena {0x1fcec0 - base} bytes" in report and "permanent core" in report
        headroom = int(report.split("level forest")[1].split("headroom ")[1].split()[0])

        # The ring first, on the block's 2 KiB boundary; the rest after it in order.
        afs = (d / "out/core.afs").read_bytes()
        bank, ring = entries(afs)
        assert bank[:3] == (1, "core", core) and bank[3:5] == (RING, len(payload))
        assert ring == (4, "ring", b"", 0, RING, 0)
        assert struct.unpack_from("<I", afs, 20)[0] == RING + len(payload)
        afs = (d / "out/forest.afs").read_bytes()
        music, song_entry, slot = entries(afs)
        music_bytes = (len((d / "music.afb").read_bytes()) - 32 + 31) & ~31
        image = (header_image := struct.unpack_from("<I", flow, 20)[0]) + (-header_image % 32)
        assert music == (1, "music", (d / "music.afb").read_bytes(), 0, music_bytes, 0)
        assert song_entry[:5] == (2, "song", flow, music_bytes, image) and song_entry[5] > 0
        assert slot == (3, "extra", b"", music_bytes + image, 128, 0)
        block = music_bytes + image + 128
        assert struct.unpack_from("<I", afs, 20)[0] == block
        arena = 0x1fcec0 - base
        assert headroom == arena - ((base + 2047) & ~2047) + base - RING - len(payload) - block

        # A song entry: the looping MIDI fixture as an AFM, placed like a flow.
        subprocess.run([str(BUILD / "afx_compile"), str(d / "fixture.mid"), "--zones", str(d / "fixture.zones"),
                        str(d / "loop.afb"), str(d / "loop.afm"), "--loop-tick", "1500"], check=True,
                       stdout=subprocess.DEVNULL)
        (d / "songs.afsm").write_text(f"firmware {FIRMWARE}\nlevel songs out/songs.afs\n"
                                      "  bank loop loop.afb\n  song theme loop.afm\n")
        scene(d / "songs.afsm")
        loop_bank, theme = entries((d / "out/songs.afs").read_bytes())
        afm = (d / "loop.afm").read_bytes()
        image_bytes = struct.unpack_from("<I", afm, 20)[0]
        assert theme[:3] == (5, "theme", afm) and theme[4] == (image_bytes + 31) & ~31 and theme[5] > 0
        (d / "nobank.afsm").write_text(f"firmware {FIRMWARE}\nlevel songs out/songs.afs\n  song theme loop.afm\n")
        scene(d / "nobank.afsm", ok=False)

        # The level's flow needs the core bank, which only the permanent scene has.
        (d / "alone.afsm").write_text(f"firmware {FIRMWARE}\n" + level.replace("out/forest", "out/alone"))
        assert "needs bank c0de0002c0de0001 in slot 1" in scene(d / "alone.afsm", ok=False).stderr
        assert not (d / "out/alone.afs").exists()
        # Too big for the arena; two levels at once only with overlapping transitions.
        (d / "big.afsm").write_text(permanent + level.replace("slot extra 100", "slot extra 2080000")
                                    .replace("out/forest", "out/big"))
        assert "do not fit" in scene(d / "big.afsm", ok=False).stderr and not (d / "out/big.afs").exists()
        two = permanent + "".join(f"level l{n} out/l{n}.afs\n  slot song 1040000\n" for n in (1, 2))
        (d / "two.afsm").write_text(two)
        scene(d / "two.afsm")
        (d / "overlap.afsm").write_text("overlap\n" + two.replace("out/l", "out/o"))
        report = scene(d / "overlap.afsm", ok=False)
        assert "overlap l1 + l2, headroom -" in report.stdout and not (d / "out/o1.afs").exists()
        # What the loader would refuse is refused here.
        for text, message in (("level x out/x.afs\n  ring 0\n  ring 1\n", "two rings"),
                              ("level x out/x.afs\n  slot a 64\n  slot a 64\n", "two entries named a"),
                              ("level x out/x.afs\n  bank a core.afb\n  bank b core.afb\n", "one identity"),
                              ("level x out/x.afs\n  slot a\n", "cannot use this line"),
                              ("bank a core.afb\n", "cannot use this line"),
                              ("level x out/x.afs\n  flow a core.afb\n", "not a valid AFX flow")):
            (d / "bad.afsm").write_text(f"firmware {FIRMWARE}\n" + text)
            assert message in scene(d / "bad.afsm", ok=False).stderr, text
        assert subprocess.run([str(BUILD / "afx_scene")], capture_output=True).returncode == 2
    print("afx_scene manifest, AFS layout and fit check: PASS")


if __name__ == "__main__":
    main()
