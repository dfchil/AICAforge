#!/usr/bin/env python3
"""Small contract check for AFX-derived visualizer sidecars."""
from pathlib import Path
from tempfile import TemporaryDirectory
import struct

import afx_visualize as visual

with TemporaryDirectory() as directory:
    path = Path(directory) / "tone.afx"
    note = bytes((visual.NOTE_PL, 0, 0, 0, 0, 0, 0, 0))
    # A long WAIT32 is a duration, not a field mask. Real music commonly has these.
    wait = struct.pack("<BI", visual.WAIT32, 600000)
    stream = (note + wait + bytes((visual.PATCH_LEVEL, 0, 0, 0)) + wait + wait +
              note + wait + bytes((visual.NOTE_PL, 1, 0, 0, 0, 0, 0, 0)) + wait +
              bytes((visual.KEYOFF, 0, visual.KEYOFF, 1)) + wait + bytes((0,)))
    image = bytearray(visual.SETUP_BYTES + len(stream))
    struct.pack_into("<H", image, visual.PITCH * 2, 0)
    struct.pack_into("<H", image, visual.TOTAL_LEVEL * 2, 0)
    image[visual.SETUP_BYTES:] = stream
    header = [visual.AFX_MAGIC, 5, 96 + len(image), 0, 96, len(image), visual.SETUP_BYTES, len(stream),
              0, 1, 0, 0, 0, 0, 0, 0, 1, 1000000, 1, 0]
    path.write_bytes(visual.AFX_HEADER.pack(*header) + bytes(96 - visual.AFX_HEADER.size) + image)
    path.write_bytes(visual.afx_metadata.attach(path.read_bytes(), {'visual_note_keys':[72, 72, 72]}))
    data = visual.build(path)
    magic, version, bands, rate, reserved, frames = visual.HEADER.unpack_from(data)
    assert (magic, version, bands, rate, reserved) == (visual.MAGIC, 1, 32, 60, 0)
    assert frames == 216 and len(data) == visual.HEADER.size + frames * bands
    payload = data[visual.HEADER.size:]
    assert max(payload) == 255 and max(payload[visual.BANDS // 2::bands]) == 255
    heights = payload[visual.BANDS // 2::bands]
    assert all(a > b for a, b in zip(heights[:36], heights[1:37])), "decay starts immediately"
    assert abs(heights[36] / heights[0] - 0.5) < 0.01, "half-height after 0.6 seconds, even with a PATCH"
    assert abs(heights[72] / heights[0] - 0.25) < 0.01, "quarter-height after 1.2 seconds"
    assert heights[108] == heights[0], "retrigger resets note age"
    assert heights[144] > heights[108], "overlapping notes contribute independently"
    assert not any(heights[180:]), "key-off clears the target; the player supplies the release fade"
    assert visual.band(80.0, 80.0, 5000.0) == 0
    assert visual.band(5000.0, 80.0, 5000.0) == visual.BANDS - 1
