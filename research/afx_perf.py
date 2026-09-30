#!/usr/bin/env python3
"""Read, validate and apply timing-preserving AFX performance sidecars."""

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

import afx_visualize as visual


FORMAT, VERSION = "aicaflow.afp", 1
AFX_FILE_VERSION = 7
TEMPLATE_ID = re.compile(r"[a-z][a-z0-9_-]{0,63}")
PARAMETERS = {"lfo": 7, "dsp_send": 8, "direct": 9, "mix": visual.TOTAL_LEVEL}


def canonical(data: bytes) -> bytes:
    """Return the complete fixed-layout bank-bound AFX bytes."""
    return data


def base(data: bytes) -> dict:
    image = canonical(data)
    if len(image) < visual.AFX_HEADER.size:
        raise ValueError("truncated AFX header")
    magic, abi, total, _, image_offset, image_size, stream_offset, stream_size, *_ = visual.AFX_HEADER.unpack_from(image)
    if (magic != visual.AFX_MAGIC or abi != AFX_FILE_VERSION or total != len(image) or
            image_offset + image_size != len(image)):
        raise ValueError("invalid AFX container")
    if stream_offset + stream_size > image_size:
        raise ValueError("invalid AFX stream bounds")
    return {"abi": abi, "canonical_sha256": hashlib.sha256(image).hexdigest()}


def notes(data: bytes) -> list[dict]:
    """Return timing-bearing note identities without treating time as editable state."""
    image = canonical(data)
    header = visual.AFX_HEADER.unpack_from(image)
    image_offset, image_size, stream_offset, stream_size = header[4:8]
    start, end = image_offset + stream_offset, image_offset + stream_offset + stream_size
    tick, cursor, ordinal_at_tick, ordinal_tick, result = 0, start, 0, None, []
    while cursor < end:
        opcode, size, channel, _, mask, _ = visual.event(image, cursor)
        if cursor + size > end or mask >> visual.FIELDS:
            raise ValueError("invalid AFX event bounds")
        if opcode in (visual.WAIT8, visual.WAIT16, visual.WAIT32):
            tick += mask
        elif opcode in (visual.NOTE, visual.NOTE_PL):
            if ordinal_tick != tick:
                ordinal_tick, ordinal_at_tick = tick, 0
            fields = [name for name, field in PARAMETERS.items() if mask & (1 << field)]
            result.append({"kind": "note", "tick": tick, "ordinal": ordinal_at_tick,
                           "channel": channel, "fields": fields, "_offset": cursor,
                           "_opcode": opcode, "_mask": mask})
            ordinal_at_tick += 1
        if opcode in (0, 0x13):
            if cursor + size != end:
                raise ValueError("AFX terminal event is not final")
            break
        cursor += size
    if cursor >= end:
        raise ValueError("AFX stream has no terminal event")
    return result


def public_notes(data: bytes) -> list[dict]:
    return [{key: value for key, value in note.items() if not key.startswith("_")} for note in notes(data)]


def empty(data: bytes) -> dict:
    return {"format": FORMAT, "version": VERSION, "base": base(data), "templates": {}, "assignments": []}


def _event_key(event: dict) -> tuple:
    if not isinstance(event, dict) or set(event) != {"kind", "tick", "ordinal", "channel"} or event["kind"] != "note":
        raise ValueError("assignment event must identify one note by kind, tick, ordinal and channel")
    if any(type(event[name]) is not int or event[name] < 0 for name in ("tick", "ordinal", "channel")) or event["channel"] >= 64:
        raise ValueError("assignment event has invalid coordinates")
    return event["kind"], event["tick"], event["ordinal"], event["channel"]


def validate(document: object, data: bytes) -> dict:
    if not isinstance(document, dict) or set(document) != {"format", "version", "base", "templates", "assignments"}:
        raise ValueError(".afp must contain format, version, base, templates and assignments")
    if document["format"] != FORMAT or document["version"] != VERSION or document["base"] != base(data):
        raise ValueError(".afp belongs to a different AFX base")
    templates, assignments = document["templates"], document["assignments"]
    if not isinstance(templates, dict) or not isinstance(assignments, list):
        raise ValueError(".afp templates and assignments must be an object and an array")
    checked_templates = {}
    for template_id, template in templates.items():
        if not isinstance(template_id, str) or not TEMPLATE_ID.fullmatch(template_id):
            raise ValueError("template ids must use lowercase letters, digits, _ or -")
        if not isinstance(template, dict) or set(template) != {"label", "parameters"}:
            raise ValueError(f"template {template_id} must contain label and parameters")
        label, parameters = template["label"], template["parameters"]
        if not isinstance(label, str) or not label.strip() or len(label) > 120 or not isinstance(parameters, dict) or not parameters:
            raise ValueError(f"template {template_id} is invalid")
        if set(parameters) - set(PARAMETERS) or any(type(value) is not int or not 0 <= value <= 0xffff
                                                     for value in parameters.values()):
            raise ValueError(f"template {template_id} has unsupported register parameters")
        checked_templates[template_id] = {"label": label.strip(), "parameters": dict(parameters)}
    available = {("note", note["tick"], note["ordinal"], note["channel"]): note for note in notes(data)}
    checked_assignments, assigned = [], set()
    for assignment in assignments:
        if not isinstance(assignment, dict) or set(assignment) != {"event", "template"}:
            raise ValueError("each assignment must contain event and template")
        key = _event_key(assignment["event"])
        if key not in available or key in assigned or assignment["template"] not in checked_templates:
            raise ValueError("assignment references an unknown or duplicate event/template")
        fields = {PARAMETERS[name] for name in checked_templates[assignment["template"]]["parameters"]}
        if any(not available[key]["_mask"] & (1 << field) for field in fields):
            raise ValueError("template requests a register field absent from its assigned NOTE event")
        assigned.add(key)
        checked_assignments.append({"event": dict(assignment["event"]), "template": assignment["template"]})
    return {"format": FORMAT, "version": VERSION, "base": base(data), "templates": checked_templates,
            "assignments": checked_assignments}


def apply(document: object, data: bytes) -> bytes:
    """Rewrite only existing NOTE register words; waits and event lengths stay identical."""
    checked = validate(document, data)
    if not checked["assignments"]:
        return data
    image = bytearray(canonical(data))
    lookup = {("note", note["tick"], note["ordinal"], note["channel"]): note for note in notes(image)}
    for assignment in checked["assignments"]:
        note = lookup[_event_key(assignment["event"])]
        parameters = checked["templates"][assignment["template"]]["parameters"]
        value_offset = note["_offset"] + (4 if note["_opcode"] == visual.NOTE_PL else 8)
        fields = [field for field in range(visual.FIELDS) if note["_mask"] & (1 << field)]
        offsets = {field: value_offset + index * 2 for index, field in enumerate(fields)}
        for name, value in parameters.items():
            struct.pack_into("<H", image, offsets[PARAMETERS[name]], value)
    header = list(visual.AFX_HEADER.unpack_from(image))
    # control_id deliberately excludes itself, so an offline profile transform
    # produces a new AFX identity without changing timing or resident layout.
    header[8] = struct.unpack_from("<I", hashlib.sha256(
        struct.pack("<5I", header[3], header[16], header[17], header[10], header[11]) +
        image[header[12]:header[12] + header[13] * 12] + image[header[4]:]).digest())[0] or 1
    visual.AFX_HEADER.pack_into(image, 0, *header)
    return bytes(image)


def load(path: Path, data: bytes) -> dict:
    return validate(json.loads(path.read_text()), data)


def save(path: Path, document: object, data: bytes) -> dict:
    checked = validate(document, data)
    if path.suffix != ".afp":
        raise ValueError("performance sidecar must have a .afp suffix")
    path.write_text(json.dumps(checked, indent=2, sort_keys=True) + "\n")
    return checked


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    inspected = commands.add_parser("inspect", help="print base identity and editable AFX note events")
    inspected.add_argument("afx", type=Path)
    initialized = commands.add_parser("init", help="write an empty sidecar bound to one AFX")
    initialized.add_argument("afx", type=Path); initialized.add_argument("output", type=Path)
    applied = commands.add_parser("apply", help="write a timing-identical AFX derived from a sidecar")
    applied.add_argument("afx", type=Path); applied.add_argument("performance", type=Path); applied.add_argument("output", type=Path)
    args = parser.parse_args()
    data = args.afx.read_bytes()
    if args.command == "inspect":
        result = {"base": base(data), "notes": public_notes(data)}
    elif args.command == "init":
        result = save(args.output, empty(data), data)
    else:
        result = {"output": str(args.output), "base": base(data)}
        args.output.write_bytes(apply(load(args.performance, data), data))
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
