"""Profile transforms retain periodic, fully reconstructed seek checkpoints."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

BIN = Path(os.environ.get("AFX_BUILD_DIR", "build")).resolve()

with tempfile.TemporaryDirectory(prefix="afx-checkpoints-") as directory:
    root = Path(directory)
    base, bank, seek, profile = (root / name for name in ("base.afx", "base.afb", "base.afc", "test.afp"))
    subprocess.run([BIN / "afx_demo_assets", "quickstart", bank, base], check=True)
    subprocess.run([BIN / "afx_profile", "init", base, profile, "dry", "0"], check=True)
    original = json.loads(profile.read_text())
    assert struct.unpack_from("<I", seek.read_bytes(), 40)[0] == 1
    rebuilt = root / "rebuilt.afc"
    subprocess.run([BIN / "afx_compile", "--seek", base, rebuilt, "--checkpoint-seconds", "1"], check=True)
    assert struct.unpack_from("<I", rebuilt.read_bytes(), 40)[0] == 9
    for seconds, count in (("2", 5), ("4294967295", 1)):
        subprocess.run([BIN / "afx_compile", "--seek", base, rebuilt,
                        "--checkpoint-seconds", seconds], check=True)
        assert struct.unpack_from("<I", rebuilt.read_bytes(), 40)[0] == count
    for bad in ("0", "-1", "1x", "4294967296", "", "1.5"):
        before = rebuilt.read_bytes()
        assert subprocess.run([BIN / "afx_compile", "--seek", base, rebuilt,
                               "--checkpoint-seconds", bad], capture_output=True).returncode != 0
        assert rebuilt.read_bytes() == before
    for mode in ("unchanged", "defaults", "templates"):
        settings = json.loads(json.dumps(original))
        if mode == "defaults":
            settings["defaults"] = {"lfo": 19024}
        if mode == "templates":
            settings["templates"] = {"test": {"parameters": {"lfo": 19024}}}
            settings["setup_templates"] = {"0": "test"}
        profile.write_text(json.dumps(settings))
        out, afc = root / f"{mode}.afx", root / f"{mode}.afc"
        subprocess.run([BIN / "afx_profile", "apply", base, seek, profile, out, afc], check=True)
        data = afc.read_bytes()
        assert struct.unpack_from("<I", data, 44)[0] == 0
        assert struct.unpack_from("<I", data, 40)[0] == 1
        if mode == "unchanged":
            assert data == seek.read_bytes() and out.read_bytes() == base.read_bytes()
        subprocess.run([BIN / "afx_profile", "apply", base, seek, profile, out, afc,
                        "--checkpoint-seconds", "1"], check=True)
        data = afc.read_bytes()
        assert struct.unpack_from("<I", data, 40)[0] == 9
        cursor = 48
        for second in range(9):
            tick, position, remaining, count = struct.unpack_from("<4I", data, cursor)
            assert tick == second * 1000
            cursor += 16
            for _ in range(count):
                words = struct.unpack_from("<I18H", data, cursor)
                if mode != "unchanged":
                    assert words[8] == 19024
                cursor += 40
            if second < 4:
                assert count == 1 and remaining == 4320 - tick
        assert cursor == len(data)
    for bad in ("0", "-1", "1x", "4294967296"):
        before = afc.read_bytes()
        assert subprocess.run([BIN / "afx_profile", "apply", base, seek, profile, out, afc,
                               "--checkpoint-seconds", bad], capture_output=True).returncode != 0
        assert afc.read_bytes() == before
print("Profile checkpoint regeneration passed")
