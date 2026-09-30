#!/usr/bin/env python3
"""Lower a resolved OoT AudioSeq trace into an AFB, AFX, AFC and AFV."""
from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path

import afx_visualize

FIELDS = {"pitch": 6, "lfo": 7, "dsp_send": 8, "direct": 9, "total_level": 10}
# Keep a single ARM7 deadline below the bounds enforced by the SH-4 host.
MAX_DECODED_COMMANDS = 38
MAX_REGISTER_WRITES = 171


def compiler():
    """Load the full AFX compiler only for flow emission.

    Sample discovery is also used by OoT's bank builder and must not require
    MIDI authoring dependencies such as mido.
    """
    import afx_compile
    return afx_compile


def fail(message: str) -> None:
    raise ValueError(f"OoT trace: {message}")


def load(path: Path) -> dict:
    try:
        trace = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"OoT trace: {error}") from error
    if not isinstance(trace, dict) or not isinstance(trace.get("samples"), list) or not isinstance(trace.get("events"), list):
        fail("root needs samples and events arrays")
    if not isinstance(trace.get("tick_rate", 1000), int) or trace.get("tick_rate", 1000) <= 0:
        fail("tick_rate must be positive")
    return trace


def samples(trace: dict, base: Path) -> tuple[list[dict], dict[str, int]]:
    afx = compiler()
    result, ids = [], {}
    for sample in trace["samples"]:
        if not isinstance(sample, dict): fail("sample must be an object")
        key, name = sample.get("id"), sample.get("path")
        if not isinstance(key, str) or not key or key in ids or not isinstance(name, str): fail("sample needs unique id and path")
        raw = (base / name).read_bytes()
        frames, fmt = sample.get("frames"), sample.get("format")
        if not isinstance(frames, int) or not 1 <= frames <= 65535 or fmt not in (afx.PCM16, afx.PCM8, afx.ADPCM):
            fail(f"sample {key}: invalid frames or format")
        expected = frames * (2 if fmt == afx.PCM16 else 1 if fmt == afx.PCM8 else 1)
        if fmt == afx.ADPCM: expected = (frames + 1) // 2
        if len(raw) != expected: fail(f"sample {key}: {len(raw)} bytes do not match format/frame count")
        loop = bool(sample.get("loop", False))
        start, end = int(sample.get("loop_start", 0)), int(sample.get("loop_end", frames - 1))
        if not (0 <= start <= end < frames): fail(f"sample {key}: invalid loop bounds")
        ids[key] = len(result)
        result.append({"raw": raw, "format": fmt, "frames": frames, "loop": loop,
                       "loop_start": start, "loop_end": end})
    return result, ids


def setup(event: dict, sample: dict, sample_offset: int) -> bytes:
    state = [0] * 18
    state[0] = (0x0200 if sample["loop"] else 0) | (sample["format"] << 7) | ((sample_offset >> 16) & 0x7f)
    state[1], state[2], state[3] = sample_offset & 0xffff, sample["loop_start"], sample["loop_end"]
    # These are the existing neutral AICAflow voice defaults.  A zero attack
    # rate never reaches an audible envelope level; zeroed filter words can
    # likewise close the voice.  AudioSeq supplies musical control data, not
    # AICA register defaults, so install a known-open baseline first.
    state[4], state[5] = 0x001f, 0x001f
    state[10], state[11:16] = 0x0024, [0x1fff] * 5
    for name, field in (("env_ad", 4), ("env_dr", 5), ("lfo", 7), ("dsp_send", 8), ("direct", 9), ("total_level", 10)):
        if name not in event: continue
        value = event[name]
        if not isinstance(value, int) or not 0 <= value <= 0xffff: fail(f"{name} must be u16")
        # MIX's low byte is the neutral LPF baseline; OoT supplies the TL high
        # byte only.  The other fields are complete AICA words.
        state[field] = value | 0x24 if name == "total_level" else value
    return struct.pack("<18H", *state)


def aica_pitch(freq: int) -> int:
    """Pack Hz exactly as KOS's AICA channel command path does."""
    if freq <= 0: fail("trace frequency must be positive")
    base, octave = 5_644_800, 7
    while freq < base and octave > -8:
        base >>= 1
        octave -= 1
    return (octave & 15) << 11 | ((freq << 10) // base & 1023)


def aica_level(volume: int) -> int:
    """Convert KOS's linear 0..255 volume to its exact AICA TL value."""
    if not 0 <= volume <= 255: fail("trace volume must be 0..255")
    attenuation = 255 if not volume else min(255, math.floor(16 * math.log2(255 / volume)))
    return attenuation << 8


def aica_direct(pan: int) -> int:
    """Pack an audible direct path and KOS's 0=left..255=right pan."""
    if not 0 <= pan <= 255: fail("trace pan must be 0..255")
    # Bits 8..12 are DISDL: zero is a muted direct path.  The old trace
    # prototype filled only DIPAN (the low five bits), so the visualizer moved
    # while every sampled voice was silent.
    if pan == 128: return 0x0f00
    return 0x0f00 | (0x10 | ((127 - pan) >> 3) if pan < 128 else (pan - 128) >> 3)


def checkpoints(events: list[tuple], batches: list[tuple], stream_offset: int, stream_end: int) -> bytes:
    """Snapshot resolved voice words once per second for the SH-4 seek path."""
    if not events: return b""
    active, plan, event_cursor, batch_cursor = {}, [], 0, 0
    for tick in range(0, events[-1][0] + 1, 1000):
        while event_cursor < len(events) and events[event_cursor][0] <= tick:
            _, kind, channel, _, payload = events[event_cursor]
            if kind == 0: active.pop(channel, None)
            elif kind == 1: active[channel] = payload[3].copy()
            elif channel in active:
                for field, value in payload.items(): active[channel][field] = value
            event_cursor += 1
        while batch_cursor < len(batches) and batches[batch_cursor][0] <= tick: batch_cursor += 1
        if batch_cursor < len(batches):
            next_tick, position = batches[batch_cursor]
            position, remaining = stream_offset + position, next_tick - tick
        else: position, remaining = stream_offset + stream_end, 0
        plan.append({"tick": tick, "stream_position": position, "local_tick": remaining,
                     "active": sorted(active.items())})
    return afx.encode_checkpoints(plan, lambda state: state)


def spread_execution_budget(events: list[tuple]) -> list[tuple]:
    """Move only overfull same-tick register groups into following 1 ms ticks."""
    loads: dict[int, tuple[int, int]] = {}
    previous: dict[int, tuple[int, int]] = {}
    scheduled = []
    for event in events:
        tick, kind, channel, _, payload = event
        writes = 19 if kind == 1 else 1 if kind == 0 else len(payload)
        if writes > MAX_REGISTER_WRITES:
            fail("one event exceeds AICA's register-write budget")
        when = tick
        if channel in previous:
            source_tick, last_when = previous[channel]
            when = max(when, last_when)
            if source_tick < tick and when == last_when:
                when += 1
        while True:
            commands, used_writes = loads.get(when, (0, 0))
            if commands < MAX_DECODED_COMMANDS and used_writes + writes <= MAX_REGISTER_WRITES:
                break
            when += 1
        loads[when] = commands + 1, used_writes + writes
        previous[channel] = tick, when
        scheduled.append((when, kind, channel, event[3], payload))
    return sorted(scheduled, key=lambda item: (item[0], item[1], item[2], item[3]))


def lower_flow(trace: dict, sample_ids: dict[str, int], bank_samples: list[dict], sample_offsets: list[int],
               bank_low: int, bank_high: int) -> tuple[bytes, bytes]:
    """Lower one resolved trace against an already-built shared sample bank."""
    afx = compiler()
    setups, setup_ids, events, max_channel, end = [], {}, [], -1, 0
    for order, event in enumerate(trace["events"]):
        if not isinstance(event, dict): fail("event must be an object")
        tick, op, channel = event.get("tick"), event.get("op"), event.get("channel")
        if not isinstance(tick, int) or tick < 0 or op not in ("note", "patch", "keyoff") or not isinstance(channel, int) or not 0 <= channel < 64:
            fail("event needs non-negative tick, note/patch/keyoff op and channel 0..63")
        max_channel, end = max(max_channel, channel), max(end, tick)
        if op == "keyoff": events.append((tick, 0, channel, order, None)); continue
        if op == "patch":
            values = event.get("values")
            if not isinstance(values, dict) or not values: fail("patch needs non-empty values object")
            fields = {}
            for name, value in values.items():
                if name not in FIELDS or not isinstance(value, int) or not 0 <= value <= 0xffff: fail("invalid patch field")
                fields[FIELDS[name]] = value
            events.append((tick, 2, channel, order, fields)); continue
        sample_id = event.get("sample")
        if sample_id not in sample_ids: fail("note references unknown sample")
        sample_index = sample_ids[sample_id]
        key = (sample_id, setup(event, bank_samples[sample_index], sample_offsets[sample_index]))
        index = setup_ids.get(key)
        if index is None: index = setup_ids.setdefault(key, len(setups)); setups.append(key)
        pitch, level = event.get("pitch"), event.get("total_level")
        if not all(isinstance(value, int) and 0 <= value <= 0xffff for value in (pitch, level)): fail("note needs u16 pitch and total_level")
        # NOTE carries the complete MIX register, not merely TL.  Preserve the
        # neutral LPF value used by KOS (and by setup()) or every note closes
        # its own filter when it replaces the setup word.
        level |= 0x24
        state = list(struct.unpack("<18H", setups[index][1]))
        state[6], state[10] = pitch, level
        events.append((tick, 1, channel, order, (index, pitch, level, state)))
    events.sort(key=lambda item: (item[0], item[1], item[2], item[3]))
    events = spread_execution_budget(events)
    if events: end = max(end, max(event[0] for event in events))
    def encode(event):
        _, kind, channel, _, payload = event
        if kind == 0: return bytes((afx.AFX_OP_KEYOFF, channel))
        if kind == 2: return afx.encode_patch(channel, payload)
        index, pitch, level, _ = payload
        return afx.encode_note(channel, index, pitch, level)
    stream, batches, stream_end = afx.build_stream(events, encode, end)
    setup_bytes = b"".join(value for _, value in setups)
    flow = afx.build_bank_flow(setup_bytes, [sample_ids[key] for key, _ in setups], bank_samples, sample_offsets, bank_low, bank_high,
                               b"", stream, afx.AFX_FLAG_MUSIC, max_channel + 1, trace.get("tick_rate", 1000))
    return flow, afx.build_seek_index(flow, checkpoints(events, batches, len(setups) * 36, stream_end))


def build_collection(inputs: list[tuple[dict, Path]]) -> tuple[bytes, list[bytes], list[bytes]]:
    """Build one AFB and matching AFX/AFC pairs, sharing byte-identical samples."""
    catalog, catalog_by_sample, traces = [], {}, []
    for trace, source in inputs:
        local, ids = samples(trace, source.parent)
        local_to_bank = {}
        for name, index in ids.items():
            sample = local[index]
            signature = (sample["raw"], sample["format"], sample["frames"], sample["loop"],
                         sample["loop_start"], sample["loop_end"])
            bank_index = catalog_by_sample.get(signature)
            if bank_index is None:
                bank_index = len(catalog)
                catalog_by_sample[signature] = bank_index
                catalog.append(sample)
            local_to_bank[name] = bank_index
        traces.append((trace, local_to_bank))
    bank, offsets, low, high = afx.build_bank_payload(catalog)
    flows, seeks = zip(*(lower_flow(trace, sample_ids, catalog, offsets, low, high)
                         for trace, sample_ids in traces)) if traces else ((), ())
    return bank, list(flows), list(seeks)


def build(trace: dict, source: Path) -> tuple[bytes, bytes, bytes]:
    """Build one complete bank-bound flow; the single-track convenience API."""
    bank, flows, seeks = build_collection([(trace, source)])
    return bank, flows[0], seeks[0]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("output", type=Path, help="base output path, without extension")
    args = parser.parse_args(argv)
    trace = load(args.trace)
    bank, flow, seek = build(trace, args.trace)
    args.output.with_suffix(".afb").write_bytes(bank)
    args.output.with_suffix(".afx").write_bytes(flow)
    if seek: args.output.with_suffix(".afc").write_bytes(seek)
    afx_visualize.write(args.output.with_suffix(".afx"), args.output.with_suffix(".afv"))
    return 0


if __name__ == "__main__": raise SystemExit(main())
