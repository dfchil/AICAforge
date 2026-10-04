"""DSP demo voices must reach MIXS0; Wilhelm must retain its 22.05 kHz rate."""
import os
from pathlib import Path
import subprocess
import struct
from tempfile import TemporaryDirectory

import afx_visualize as visual

root = Path(__file__).resolve().parents[1]
binary = Path(os.environ.get("AFX_BUILD_DIR", root / "build")) / "afx_demo_assets"
with TemporaryDirectory() as directory:
    output = Path(directory)
    pcm = output / "wilhelm.pcm"
    pcm.write_bytes(bytes(256))
    subprocess.run([str(binary), "dsp-effects", str(output), str(pcm)], check=True)
    bank = (output / "inputs.afb").read_bytes()
    assert list(output.glob("*.afb")) == [output / "inputs.afb"]
    seen = {}
    for name in ("effect", "impulse", "tone", "modulated", "wilhelm"):
        path = output / (name + ".afx")
        data = path.read_bytes()
        assert data[40:48] == bank[8:16], "all flows bind to the same bank"
        relocations, count = struct.unpack_from("<II", data, 48)
        payload = struct.unpack_from("<I", bank, 20)[0]
        for i in range(count):
            _, offset, size = struct.unpack_from("<III", data, relocations + 12 * i)
            assert offset + size <= payload
        actions, duration, _, _, setups = visual.decode(path)
        buses, pitches, last_off = [], [], 0
        for tick, (opcode, _, setup, mask, values) in actions:
            if opcode == visual.KEYOFF:
                last_off = max(last_off, tick)
            if opcode not in (visual.NOTE, visual.NOTE_PL):
                continue
            state = setups[setup].copy()
            visual.apply(state, mask, values.copy())
            assert state[8] >> 4 == 15, (name, "missing DSP send")
            bus = state[8] & 15
            buses.append(bus)
            pitches.append(state[visual.PITCH])
            if bus:
                assert state[9] >> 8 == 0, "controls must not be directly audible"
            if name == "wilhelm":
                assert state[visual.PITCH] == 0x7800, "22.05 kHz, no extra tuning"
        assert duration - last_off >= 1000, "leave time for DSP tails"
        seen[name] = (buses, pitches)
    assert sorted(seen["effect"][0]) == [0, 0, 0, 0, 1, 2, 3, 4]
    assert sorted(seen["modulated"][0]) == [0, 0, 0, 0, 1]
    assert seen["impulse"][1] != seen["tone"][1]
print("DSP shared bank, input voices, routing, tails and Wilhelm pitch: PASS")
