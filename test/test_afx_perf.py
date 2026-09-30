#!/usr/bin/env python3
"""Small contract check for timing-preserving .afp application."""

import struct
import tempfile
from pathlib import Path

import afx_perf
import afx_visualize as visual


mask = (1 << 7) | (1 << visual.TOTAL_LEVEL)
note = bytes((visual.NOTE, 0)) + struct.pack("<HIHH", 0, mask, 0x1234, 0x5678)
stream = note + bytes((visual.WAIT8, 9, 0))
image = bytearray(visual.SETUP_BYTES + len(stream))
image[visual.SETUP_BYTES:] = stream
header = [visual.AFX_MAGIC, 7, 96 + len(image), 0, 96, len(image), visual.SETUP_BYTES, len(stream),
          1, 1, 1, 0, 80, 1, 0, 0, 1, 1000, 1, 0]
asset = visual.AFX_HEADER.pack(*header) + bytes(96 - visual.AFX_HEADER.size) + image

document = afx_perf.empty(asset)
assert afx_perf.apply(document, asset) == asset
event = afx_perf.public_notes(asset)[0]
document["templates"] = {"wide": {"label": "Wide", "parameters": {"lfo": 0xabcd, "mix": 0x3210}}}
document["assignments"] = [{"event": {key: event[key] for key in ("kind", "tick", "ordinal", "channel")},
                            "template": "wide"}]
checked = afx_perf.validate(document, asset)
derived = afx_perf.apply(checked, asset)
assert afx_perf.canonical(asset)[-3:] == afx_perf.canonical(derived)[-3:] == bytes((visual.WAIT8, 9, 0))
changed = afx_perf.notes(derived)[0]
assert changed["tick"] == event["tick"] == 0 and changed["ordinal"] == event["ordinal"] == 0
values = afx_perf.canonical(derived)
assert struct.unpack_from("<HH", values, changed["_offset"] + 8) == (0xabcd, 0x3210)
assert afx_perf.base(asset) == checked["base"]
try:
    afx_perf.validate(checked, derived)
except ValueError:
    pass
else:
    raise AssertionError("sidecar accepted its derived asset as its base")
document["assignments"][0]["event"]["tick"] = 1
try:
    afx_perf.validate(document, asset)
except ValueError:
    pass
else:
    raise AssertionError("sidecar accepted a nonexistent event")

legacy = bytearray(asset)
struct.pack_into("<I", legacy, 4, 6)
try:
    afx_perf.empty(legacy)
except ValueError:
    pass
else:
    raise AssertionError("sidecar accepted a legacy AFX")

with tempfile.TemporaryDirectory() as directory:
    profile = Path(directory) / "song.afp"
    assert afx_perf.save(profile, afx_perf.empty(asset), asset)["format"] == "aicaflow.afp"
    try:
        afx_perf.save(profile.with_suffix(".afperf"), afx_perf.empty(asset), asset)
    except ValueError:
        pass
    else:
        raise AssertionError("legacy profile suffix accepted")

print("afp timing-preservation checks passed")
