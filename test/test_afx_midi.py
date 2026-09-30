#!/usr/bin/env python3
"""Regression check for repeated notes, sustain and source-tick preservation."""

import afx_midi
import tempfile
from pathlib import Path

import mido




with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / "repeated.mid"
    midi = mido.MidiFile(ticks_per_beat=480)
    track = mido.MidiTrack()
    midi.tracks.append(track)
    track.append(mido.MetaMessage("set_tempo", tempo=600000, time=0))
    track.append(mido.Message("program_change", channel=0, program=42, time=0))
    track.append(mido.Message("control_change", channel=0, control=7, value=64, time=0))
    track.append(mido.Message("control_change", channel=0, control=10, value=32, time=0))
    track.append(mido.Message("control_change", channel=0, control=11, value=100, time=0))
    track.append(mido.Message("control_change", channel=0, control=64, value=127, time=100))
    track.append(mido.Message("note_on", channel=0, note=60, velocity=80, time=0))
    track.append(mido.Message("note_off", channel=0, note=60, velocity=0, time=100))
    track.append(mido.Message("note_on", channel=0, note=60, velocity=90, time=50))
    track.append(mido.Message("note_off", channel=0, note=60, velocity=0, time=50))
    track.append(mido.Message("control_change", channel=0, control=64, value=0, time=100))
    track.append(mido.MetaMessage("end_of_track", time=0))
    midi.save(source)
    timeline = afx_midi.parse(source)
    assert timeline["ticks_per_beat"] == 480 and timeline["tempos"][1]["us_per_beat"] == 600000
    assert len(timeline["notes"]) == 2 and not timeline["warnings"]
    assert [note["start_tick"] for note in timeline["notes"]] == [100, 250]
    assert [note["end_tick"] for note in timeline["notes"]] == [400, 400]
    assert all(note["program"] == 42 for note in timeline["notes"])
    assert all((note["channel_volume"], note["channel_expression"], note["channel_pan"]) ==
               (64, 100, 32) for note in timeline["notes"])
    assert all((note["pitch_bend"], note["pitch_bend_range_cents"]) == (0, 200)
               for note in timeline["notes"])
    assert timeline["allocation"] == {"local_channels": 2, "peak_overlap": 2}

    contiguous = [{"start_tick": 0, "end_tick": 10}, {"start_tick": 10, "end_tick": 20}]
    assert afx_midi.allocate_local_channels(contiguous) == {"local_channels": 1, "peak_overlap": 1}
    assert [note["local_channel"] for note in contiguous] == [0, 0]

    bend_source = Path(directory) / "bend.mid"
    bend = mido.MidiFile(ticks_per_beat=480)
    bend_track = mido.MidiTrack(); bend.tracks.append(bend_track)
    bend_track.append(mido.Message("control_change", channel=0, control=101, value=0, time=0))
    bend_track.append(mido.Message("control_change", channel=0, control=100, value=0, time=0))
    bend_track.append(mido.Message("control_change", channel=0, control=6, value=12, time=0))
    bend_track.append(mido.Message("control_change", channel=0, control=38, value=50, time=0))
    bend_track.append(mido.Message("note_on", channel=0, note=60, velocity=100, time=0))
    bend_track.append(mido.Message("pitchwheel", channel=0, pitch=4096, time=120))
    bend_track.append(mido.Message("note_off", channel=0, note=60, velocity=0, time=120))
    bend_track.append(mido.MetaMessage("end_of_track", time=0)); bend.save(bend_source)
    bend_timeline = afx_midi.parse(bend_source)
    assert bend_timeline["notes"][0]["pitch_bend_range_cents"] == 1250
    assert bend_timeline["automation"][-1] == {"tick": 120, "track": 0, "order": 5,
                                                  "channel": 0, "kind": "pitch", "pitch_bend": 4096,
                                                  "pitch_bend_range_cents": 1250}

    gs_source = Path(directory) / "gs-nrpn.mid"
    gs = mido.MidiFile(ticks_per_beat=480)
    gs_track = mido.MidiTrack(); gs.tracks.append(gs_track)
    gs_track.append(mido.Message("control_change", channel=0, control=99, value=1, time=0))
    gs_track.append(mido.Message("control_change", channel=0, control=98, value=32, time=0))
    gs_track.append(mido.Message("control_change", channel=0, control=6, value=14, time=0))
    gs_track.append(mido.Message("note_on", channel=0, note=60, velocity=100, time=0))
    gs_track.append(mido.Message("control_change", channel=0, control=6, value=64, time=120))
    gs_track.append(mido.Message("control_change", channel=0, control=38, value=127, time=0))
    gs_track.append(mido.Message("note_off", channel=0, note=60, velocity=0, time=120))
    gs_track.append(mido.MetaMessage("end_of_track", time=0)); gs.save(gs_source)
    gs_timeline = afx_midi.parse(gs_source)
    assert not gs_timeline["warnings"]
    assert gs_timeline["notes"][0]["channel_brightness"] == 14
    cutoff = [event for event in gs_timeline["automation"] if event.get("kind") == "filter_cutoff"]
    assert [(event["value"], event["value14"]) for event in cutoff] == [(14, 1792), (64, 8192), (65, 8319)]
    assert cutoff[-1]["nrpn"] == [1, 32]

    panic_source = Path(directory) / "panic.mid"
    panic = mido.MidiFile(ticks_per_beat=480)
    panic_track = mido.MidiTrack(); panic.tracks.append(panic_track)
    panic_track.append(mido.Message("control_change", channel=0, control=64, value=127, time=0))
    panic_track.append(mido.Message("note_on", channel=0, note=60, velocity=100, time=0))
    panic_track.append(mido.Message("control_change", channel=0, control=123, value=0, time=100))
    panic_track.append(mido.Message("control_change", channel=0, control=64, value=0, time=100))
    panic_track.append(mido.Message("note_on", channel=0, note=62, velocity=100, time=0))
    panic_track.append(mido.Message("control_change", channel=0, control=120, value=0, time=100))
    panic_track.append(mido.MetaMessage("end_of_track", time=0)); panic.save(panic_source)
    panic_timeline = afx_midi.parse(panic_source)
    assert [note["end_tick"] for note in panic_timeline["notes"]] == [200, 300]
    assert not panic_timeline["warnings"]

    warning_source = Path(directory) / "warnings.mid"
    warned = mido.MidiFile(); warning_track = mido.MidiTrack(); warned.tracks.append(warning_track)
    warning_track.append(mido.Message("control_change", channel=0, control=121, value=90, time=0))
    warning_track.append(mido.Message("aftertouch", channel=0, value=40, time=0))
    warning_track.append(mido.Message("sysex", data=(1, 2, 3), time=0))
    warning_track.append(mido.Message("control_change", channel=0, control=93, value=64, time=0))
    warning_track.append(mido.MetaMessage("end_of_track", time=0)); warned.save(warning_source)
    warning_items = afx_midi.parse(warning_source)["warnings"]
    warning_kinds = {item["kind"] for item in warning_items}
    assert warning_kinds == {"unsupported_pressure", "unsupported_sysex"}
    assert all(item["fallback"] == "no AICA command emitted" and item["missing_capability"]
               for item in warning_items)
    chorus_timeline = afx_midi.parse(warning_source)
    assert chorus_timeline["automation"][-1]["control"] == 93
    assert chorus_timeline["automation"][-1]["channel_chorus"] == 64

    reset_source = Path(directory) / "reset.mid"
    reset = mido.MidiFile(); reset_track = mido.MidiTrack(); reset.tracks.append(reset_track)
    reset_track.append(mido.Message("program_change", channel=0, program=42, time=0))
    reset_track.append(mido.Message("control_change", channel=0, control=0, value=5, time=0))
    reset_track.append(mido.Message("control_change", channel=0, control=7, value=64, time=0))
    reset_track.append(mido.Message("control_change", channel=0, control=10, value=0, time=0))
    reset_track.append(mido.Message("control_change", channel=0, control=11, value=64, time=0))
    reset_track.append(mido.Message("control_change", channel=0, control=1, value=127, time=0))
    reset_track.append(mido.Message("pitchwheel", channel=0, pitch=4096, time=0))
    reset_track.append(mido.Message("control_change", channel=0, control=64, value=127, time=0))
    reset_track.append(mido.Message("note_on", channel=0, note=60, velocity=100, time=0))
    reset_track.append(mido.Message("note_off", channel=0, note=60, velocity=0, time=20))
    reset_track.append(mido.Message("control_change", channel=0, control=121, value=0, time=0))
    reset_track.append(mido.Message("note_on", channel=0, note=62, velocity=100, time=0))
    reset_track.append(mido.Message("note_off", channel=0, note=62, velocity=0, time=20))
    reset_track.append(mido.MetaMessage("end_of_track", time=0)); reset.save(reset_source)
    reset_timeline = afx_midi.parse(reset_source)
    assert [note["end_tick"] for note in reset_timeline["notes"]] == [20, 40]
    assert (reset_timeline["notes"][1]["bank_msb"], reset_timeline["notes"][1]["program"],
            reset_timeline["notes"][1]["channel_volume"], reset_timeline["notes"][1]["channel_pan"],
            reset_timeline["notes"][1]["channel_expression"], reset_timeline["notes"][1]["channel_modulation"],
            reset_timeline["notes"][1]["pitch_bend"]) == (5, 42, 64, 0, 127, 0, 0)
    reset_event = reset_timeline["automation"][-1]
    assert reset_event["control"] == 121 and reset_event["pitch_bend_range_cents"] == 200

print("afx MIDI timeline checks passed")
