#!/usr/bin/env python3
"""Offline OoT AudioSeq sample discovery for AICAflow.

The OoT bank builder uses this module to determine which original sample keys
each sequence can reach.  Encoding and packing those samples belongs to the
current AFB builder; this tool intentionally has no dependency on a generated
sample pool or the retired Dreamcast synth backend.
"""
from __future__ import annotations

import argparse
import contextlib
import importlib
import io
import json
import math
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

import afx_n64_trace


TICK_RATE = 1000
MAX_SCRIPT_STEPS = 200_000
MAX_BACKWARD_JUMPS = 1
INSN = re.compile(r"/\*\s*0x[0-9A-Fa-f]+\s*\[[^]]*\]\s*\*/\s*([a-z][a-z0-9_]*)\s*(.*)")
SECTION = re.compile(r"\.(sequence|channel|layer)\s+([A-Z]+_[0-9A-F]+)")
LABEL = re.compile(r"([A-Z]+_[0-9A-F]+):")
FONT = re.compile(r"Soundfont_(\d+)\.h")
INSTRUMENT = re.compile(r"SF(\d+)_INST_(\d+)$")
PITCH = re.compile(r"PITCH_([A-Z0-9F]+)$")


def aica_dsp_send(reverb: int) -> int:
    """Map AudioSeq's 7-bit per-channel reverb volume to MIXS0's IMXL."""
    if not 0 <= reverb <= 255:
        fail("reverb must be a byte")
    return ((reverb & 0x7F) * 15 + 63) // 127 << 4


def fail(message: str) -> None:
    raise ValueError(f"OoT AudioSeq: {message}")


def number(value: str) -> int:
    value = value.strip()
    if value.startswith("0b"):
        return int(value[2:], 2)
    return int(value, 0)


def arguments(value: str) -> list[str]:
    return [part.strip() for part in value.split(",")] if value else []


@dataclass
class Instruction:
    name: str
    args: list[str]


@dataclass
class Program:
    kind: str
    name: str
    ops: list[Instruction] = field(default_factory=list)
    labels: dict[str, int] = field(default_factory=dict)


@dataclass
class Channel:
    index: int
    font: int
    volume: int = 127
    pan: int = 64
    transpose: int = 0
    reverb: int = 0
    instrument: int | None = None
    layers: dict[int, "Script"] = field(default_factory=dict)
    script: "Script | None" = None


@dataclass
class Script:
    program: Program
    pc: int = 0
    due: int = 0
    stack: list[tuple[Program, int]] = field(default_factory=list)
    jumps: dict[tuple[str, int], int] = field(default_factory=dict)
    value: int = 0
    active: bool = True
    channel: Channel | None = None
    layer: int | None = None
    transpose: int = 0
    last_delay: int = 0


@dataclass
class Source:
    oot_root: Path
    fonts: list[object]
    default_fonts: dict[int, int]
    pitches: tuple[float, ...]
    pitch_names: tuple[str, ...]


def oot_modules(oot_root: Path) -> tuple[object, object, object, object, object, object, object]:
    tools = str(oot_root / "tools")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    try:
        version_config = importlib.import_module("version_config")
        tables = importlib.import_module("audio.extraction.audio_tables")
        extract = importlib.import_module("audio.extraction.audio_extract")
        disassembler = importlib.import_module("audio.extraction.disassemble_sequence")
        util = importlib.import_module("audio.extraction.util")
        tuning = importlib.import_module("audio.extraction.tuning")
    except ImportError as error:
        fail(f"cannot import OoT's extraction code from {tools}: {error}")
    return version_config, tables, extract, disassembler, util, tuning, tools


def source(oot_root: Path) -> tuple[Source, object, object, object, object, object]:
    oot_root = oot_root.resolve()
    base = oot_root / "extracted/gc-eu-mq-dbg/baserom"
    required = (base / "code", base / "Audiobank", base / "Audiotable", base / "Audioseq")
    if any(not path.is_file() for path in required):
        fail(f"{oot_root}: expected extracted gc-eu-mq-dbg audio assets")
    version_config, tables, extract, disassembler, util, tuning, _ = oot_modules(oot_root)
    config = version_config.load_version_config("gc-eu-mq-dbg")
    code = memoryview((base / "code").read_bytes())
    audiobank = memoryview((base / "Audiobank").read_bytes())
    audiotable = memoryview((base / "Audiotable").read_bytes())
    audioseq = memoryview((base / "Audioseq").read_bytes())
    code_base = config.dmadata_segments["code"].vram
    offset = lambda name: config.variables[name] - code_base
    soundfont_table = tables.AudioCodeTable(code, offset("gSoundFontTable"))
    samplebank_table = tables.AudioCodeTable(code, offset("gSampleBankTable"))
    sequence_table = tables.AudioCodeTable(code, offset("gSequenceTable"))
    sequence_font_table = util.incbin(code, offset("gSequenceFontTable"),
                                      offset("gSequenceTable") - offset("gSequenceFontTable"))
    specs = {
        0: tuple(disassembler.SequenceTableSpec(offset, count, addend, section) for offset, count, addend, section in (
            (0x00E1, 128, 0, disassembler.SqSection.CHAN), (0x0EE3, 80, 0, disassembler.SqSection.CHAN),
            (0x16D5, 248, 0, disassembler.SqSection.CHAN), (0x315E, 499, 0, disassembler.SqSection.CHAN),
            (0x5729, 72, 0, disassembler.SqSection.CHAN), (0x5EE5, 8, 0, disassembler.SqSection.CHAN),
            (0x5FF2, 128, 0, disassembler.SqSection.CHAN))),
        1: tuple(disassembler.SequenceTableSpec(offset, 20, addend, disassembler.SqSection.LAYER)
                 for offset, addend in ((0x0192, 0), (0x01BA, 0), (0x01E2, 0), (0x020A, 0),
                                         (0x0232, 1), (0x025A, 1), (0x0282, 1))),
        2: (disassembler.SequenceTableSpec(0x00BC, 2, 0, disassembler.SqSection.SEQ),
            disassembler.SequenceTableSpec(0x00C0, 2, 0, disassembler.SqSection.ARRAY)),
        109: (disassembler.SequenceTableSpec(0x0646, 16, 0, disassembler.SqSection.CHAN),),
    }
    info = extract.GameVersionInfo("gc-eu-mq-dbg", disassembler.MMLVersion.OOT,
                                   offset("gSoundFontTable"), offset("gSequenceFontTable"),
                                   offset("gSequenceTable"), offset("gSampleBankTable"),
                                   tuple(f"Sequence_{i}" for i in range(len(sequence_table))), (), {37: 2}, (0,), specs)
    # The upstream extractor prints two known layout diagnostics for this
    # debug-ROM variant.  They are not conversion diagnostics and would make
    # the otherwise quiet reproducible build needlessly noisy.
    with contextlib.redirect_stdout(io.StringIO()):
        banks = extract.collect_sample_banks(audiotable, str(oot_root), info, samplebank_table, {})
        fonts = extract.collect_soundfonts(audiobank, str(oot_root), info, soundfont_table, {}, banks)
    defaults = {}
    for sequence, _entry in enumerate(sequence_table):
        position = (sequence_font_table[2 * sequence] << 8) | sequence_font_table[2 * sequence + 1]
        count = sequence_font_table[position]
        if count:
            defaults[sequence] = int(sequence_font_table[position + count])
    pitches = tuple(struct.unpack(">f", struct.pack(">I", value))[0] for value in tuning.g_pitch_frequencies)
    return Source(oot_root, fonts, defaults, pitches, tuple(tuning.pitch_names)), \
        sequence_table, sequence_font_table, audioseq, disassembler, info


def disassemble(src: Source, sequence_table: object, sequence_fonts: memoryview, audioseq: memoryview,
                disassembler: object, info: object, sequence: int, output: Path) -> Path:
    if not 0 <= sequence < len(sequence_table):
        fail(f"sequence {sequence} is outside the source table")
    entry = sequence_table.entries[sequence]
    if entry.size == 0:
        entry = sequence_table.entries[entry.rom_addr]
    position = (sequence_fonts[2 * sequence] << 8) | sequence_fonts[2 * sequence + 1]
    count = sequence_fonts[position]
    font_ids = sequence_fonts[position + 1:position + 1 + count]
    output.parent.mkdir(parents=True, exist_ok=True)
    tables = info.seq_disas_tables.get(sequence, None)
    program = disassembler.SequenceDisassembler(sequence, bytearray(entry.data(audioseq, sequence_table.rom_addr)), tables,
                                                disassembler.CMD_SPEC, disassembler.MMLVersion.OOT, str(output),
                                                f"Sequence_{sequence}", [src.fonts[index] for index in font_ids],
                                                tuple(f"Sequence_{index}" for index in range(len(sequence_table))))
    program.analyze()
    program.emit()
    return output


def programs(path: Path) -> tuple[dict[str, Program], int]:
    found: dict[str, Program] = {}
    current: Program | None = None
    default_font: int | None = None
    for raw in path.read_text().splitlines():
        if match := FONT.search(raw):
            default_font = int(match.group(1))
        if match := SECTION.fullmatch(raw.strip()):
            kind, name = match.groups()
            current = Program(kind, name)
            current.labels[name] = 0
            found[name] = current
            continue
        if match := LABEL.fullmatch(raw.strip()):
            if current is None:
                fail(f"{path}: label outside a script")
            current.labels[match.group(1)] = len(current.ops)
            continue
        if match := INSN.search(raw):
            if current is None:
                fail(f"{path}: instruction outside a script")
            current.ops.append(Instruction(match.group(1), arguments(match.group(2))))
    if not found or default_font is None:
        fail(f"{path}: missing scripts or default soundfont")
    labels = {label for program in found.values() for label in program.labels}
    for program in list(found.values()):
        for label in labels:
            if label not in found and label in program.labels:
                found[label] = program
    return found, default_font


def locate(programs_by_name: dict[str, Program], label: str) -> tuple[Program, int]:
    seen = set()
    for program in programs_by_name.values():
        if id(program) in seen:
            continue
        seen.add(id(program))
        if label in program.labels:
            return program, program.labels[label]
    fail(f"unknown script label {label}")


def sample(src: Source, font: int, program: int, note: int) -> tuple[str, dict, float]:
    if not 0 <= font < len(src.fonts):
        fail(f"invalid soundfont {font}")
    soundfont = src.fonts[font]
    instrument = soundfont.instrument_index_map.get(program)
    if instrument is None:
        fail(f"soundfont {font} has no instrument {program}")
    if note < instrument.normal_range_lo:
        header, tuning = instrument.low_notes_sample, instrument.low_notes_tuning
    elif note > instrument.normal_range_hi:
        header, tuning = instrument.high_notes_sample, instrument.high_notes_tuning
    else:
        header, tuning = instrument.normal_notes_sample, instrument.normal_notes_tuning
    if not header or not tuning:
        fail(f"soundfont {font} instrument {program} has no sample for note {note}")
    header = soundfont.sample_headers[header]
    key = soundfont.bank1.table_entry.rom_addr + header.sample_addr
    return f"{key:06X}", {}, float(tuning)


def pitch(src: Source, name: str, tuning: float) -> int:
    name = name.removeprefix("PITCH_")
    if name not in src.pitch_names:
        fail(f"invalid note {name}")
    semitone = src.pitch_names.index(name)
    return afx_n64_trace.aica_pitch(max(1, round(32_000 * src.pitches[semitone] * tuning)))


def lower(path: Path, src: Source, sequence: int) -> dict:
    programs_by_name, default_font = programs(path)
    root, root_pc = locate(programs_by_name, "SEQ_0000")
    root_script = Script(root, root_pc)
    scripts: list[Script] = [root_script]
    channels: dict[int, Channel] = {}
    samples: dict[str, dict] = {}
    events: list[dict] = []
    releases: list[tuple[int, int]] = []
    active_voices: set[int] = set()
    tempo, time_ms, tick, steps = 120, 0.0, 0, 0

    def release_due() -> None:
        nonlocal releases
        due, releases = [entry for entry in releases if entry[0] <= tick], [entry for entry in releases if entry[0] > tick]
        for _, channel in due:
            active_voices.discard(channel)
            events.append({"tick": round(time_ms), "op": "keyoff", "channel": channel})

    def jump(script: Script, target: str) -> bool:
        program, pc = locate(programs_by_name, target)
        if program is script.program and pc <= script.pc:
            key = (program.name, pc)
            script.jumps[key] = script.jumps.get(key, 0) + 1
            if script.jumps[key] > MAX_BACKWARD_JUMPS:
                script.active = False
                return False
        script.program, script.pc = program, pc
        return True

    def step(script: Script) -> None:
        nonlocal tempo, steps
        while script.active and script.due <= tick:
            steps += 1
            if steps > MAX_SCRIPT_STEPS:
                fail(f"sequence {sequence}: script did not quiesce")
            if script.pc >= len(script.program.ops):
                if script.stack:
                    script.program, script.pc = script.stack.pop()
                    continue
                script.active = False
                return
            op = script.program.ops[script.pc]
            script.pc += 1
            name, args = op.name, op.args
            if name in ("delay", "cdelay", "ldelay"):
                script.due = tick + number(args[0])
                return
            if name == "jump":
                jump(script, args[0]); continue
            if name == "call":
                script.stack.append((script.program, script.pc))
                program, pc = locate(programs_by_name, args[0])
                script.program, script.pc = program, pc
                continue
            if name == "end":
                if script.stack:
                    script.program, script.pc = script.stack.pop()
                else:
                    script.active = False
                continue
            if name in ("beqz", "rbeqz"):
                if script.value == 0: jump(script, args[0])
                continue
            if name == "rbltz":
                if script.value < 0: jump(script, args[0])
                continue
            if name == "ldi": script.value = number(args[0]); continue
            if name == "sub": script.value -= number(args[0]); continue
            if name == "tempo":
                tempo = max(1, number(args[0]))
                continue
            if name in ("ldchan", "rldchan"):
                index, target = number(args[0]), args[1]
                # AudioSeq_SequenceChannelEnable only replaces the script and
                # layers; its channel settings (font, instrument, pan, etc.)
                # deliberately survive a later ldchan.
                channel = channels.get(index)
                if channel is None:
                    channel = Channel(index, default_font)
                    channels[index] = channel
                if channel.script is not None:
                    channel.script.active = False
                for layer_script in channel.layers.values():
                    layer_script.active = False
                channel.layers.clear()
                program, pc = locate(programs_by_name, target)
                channel.script = Script(program, pc, channel=channel)
                scripts.append(channel.script)
                continue
            if script.channel is not None:
                channel = script.channel
                if name in ("ldlayer", "rldlayer"):
                    layer, target = number(args[0]), args[1]
                    program, pc = locate(programs_by_name, target)
                    layer_script = Script(program, pc, channel=channel, layer=layer)
                    channel.layers[layer] = layer_script
                    scripts.append(layer_script)
                    continue
                if name == "instr":
                    match = INSTRUMENT.fullmatch(args[0])
                    channel.instrument = int(match.group(2)) if match else None
                    if match: channel.font = int(match.group(1))
                    continue
                if name == "vol": channel.volume = number(args[0]); continue
                if name == "pan": channel.pan = number(args[0]); continue
                if name == "transpose": channel.transpose = number(args[0]); continue
                if name == "reverb":
                    channel.reverb = number(args[0])
                    for voice in sorted(active_voices):
                        if voice // 4 == channel.index:
                            events.append({"tick": round(time_ms), "op": "patch", "channel": voice,
                                           "values": {"dsp_send": aica_dsp_send(channel.reverb)}})
                    continue
            if script.layer is not None and name in ("notedvg", "notedv", "notevg"):
                channel = script.channel
                if channel is None or channel.instrument is None:
                    continue
                if name == "notedvg":
                    note, delay, velocity, gate = args
                    script.last_delay = number(delay)
                elif name == "notedv":
                    note, delay, velocity = args
                    gate = "0"
                    script.last_delay = number(delay)
                else:
                    note, velocity, gate = args
                    delay = str(script.last_delay)
                duration = number(delay)
                key, record, tuning = sample(src, channel.font, channel.instrument,
                                             src.pitch_names.index(PITCH.fullmatch(note).group(1)) + channel.transpose + script.transpose)
                samples.setdefault(key, {"id": key, "path": f"{key}.bin", **record})
                source_note = src.pitch_names.index(PITCH.fullmatch(note).group(1)) + channel.transpose + script.transpose
                volume = max(1, min(255, round(255 * (channel.volume / 127.0) * (number(velocity) / 127.0) ** 2)))
                virtual_channel = channel.index * 4 + script.layer
                events.append({"tick": round(time_ms), "op": "note", "channel": virtual_channel, "sample": key,
                               "pitch": pitch(src, src.pitch_names[source_note], tuning),
                               "total_level": afx_n64_trace.aica_level(volume),
                               "dsp_send": aica_dsp_send(channel.reverb),
                               "direct": afx_n64_trace.aica_direct(min(255, max(0, channel.pan * 2)))})
                active_voices.add(virtual_channel)
                tail = (duration * number(gate)) >> 8
                releases.append((tick + max(1, duration - tail), virtual_channel))
                script.due = tick + max(1, duration)
                return

    while any(script.active for script in scripts) or releases:
        release_due()
        for script in tuple(scripts):
            if script.active and script.due <= tick:
                step(script)
        tick += 1
        time_ms += 60_000.0 / (tempo * 48)
        if tick > 100_000:
            fail(f"sequence {sequence}: exceeded 100000 sequence ticks")
    events.sort(key=lambda event: (event["tick"], event["op"] != "keyoff", event["channel"]))
    return {"tick_rate": TICK_RATE, "samples": list(samples.values()), "events": events}


def sample_keys(sequence: int, oot_root: Path, output: Path) -> set[int]:
    """Return the original samples reached by one OoT sequence."""
    src, sequence_table, sequence_fonts, audioseq, disassembler, info = source(oot_root)
    mml = disassemble(src, sequence_table, sequence_fonts, audioseq, disassembler, info, sequence,
                      output.with_suffix(".seq"))
    return {int(record["id"], 16) for record in lower(mml, src, sequence)["samples"]}


def sequence_trace(sequence: int, oot_root: Path, output: Path) -> dict:
    """Read one OoT AudioSeq song into the common note/patch trace.

    The trace deliberately references source sample IDs instead of embedding
    sample bytes.  An AFB builder owns packing and codec policy.
    """
    src, sequence_table, sequence_fonts, audioseq, disassembler, info = source(oot_root)
    mml = disassemble(src, sequence_table, sequence_fonts, audioseq, disassembler, info, sequence,
                      output.with_suffix(".seq"))
    return lower(mml, src, sequence)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Read an OoT AudioSeq song into an AICAflow trace")
    parser.add_argument("oot_root", type=Path, help="OoT checkout with extracted gc-eu-mq-dbg audio assets")
    parser.add_argument("sequence", type=int)
    parser.add_argument("output", type=Path, help="output trace JSON")
    args = parser.parse_args(argv)
    try:
        trace = sequence_trace(args.sequence, args.oot_root, args.output)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(trace, indent=2) + "\n")
    except (OSError, ValueError, KeyError, struct.error) as error:
        print(f"afx-n64 audioseq: {error}", file=sys.stderr)
        return 2
    print(f"wrote {args.output}: {len(trace['samples'])} samples, {len(trace['events'])} events")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
