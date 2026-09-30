#!/usr/bin/env python3
"""Generate the tiny MIDI source used by the bounded compiler hardware probe."""

import sys
from pathlib import Path

import mido


if len(sys.argv) != 2:
    raise SystemExit(f"usage: {sys.argv[0]} output.mid")
output = Path(sys.argv[1])
output.parent.mkdir(parents=True, exist_ok=True)
midi = mido.MidiFile(ticks_per_beat=480)
track = mido.MidiTrack(); midi.tracks.append(track)
track.append(mido.Message("program_change", channel=0, program=0, time=0))
track.append(mido.Message("note_on", channel=0, note=69, velocity=100, time=0))
track.append(mido.Message("control_change", channel=0, control=7, value=64, time=960))
track.append(mido.Message("control_change", channel=0, control=10, value=96, time=0))
track.append(mido.Message("control_change", channel=0, control=91, value=127, time=0))
track.append(mido.Message("control_change", channel=0, control=1, value=127, time=0))
track.append(mido.Message("control_change", channel=0, control=71, value=127, time=0))
track.append(mido.Message("control_change", channel=0, control=74, value=127, time=0))
track.append(mido.Message("pitchwheel", channel=0, pitch=4096, time=0))
track.append(mido.Message("control_change", channel=0, control=121, value=0, time=1440))
track.append(mido.Message("control_change", channel=0, control=64, value=127, time=0))
track.append(mido.Message("control_change", channel=0, control=123, value=0, time=480))
track.append(mido.Message("control_change", channel=0, control=64, value=0, time=1440))
track.append(mido.Message("note_on", channel=0, note=81, velocity=100, time=480))
track.append(mido.Message("control_change", channel=0, control=120, value=0, time=480))
track.append(mido.Message("note_on", channel=0, note=84, velocity=100, time=2400))
track.append(mido.Message("note_off", channel=0, note=84, velocity=0, time=480))
track.append(mido.MetaMessage("end_of_track", time=0))
midi.save(output)
