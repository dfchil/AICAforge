#!/usr/bin/env python3
"""Shared-bank merge contract: final AFB+AFX inputs stay final."""

import struct
import tempfile
from pathlib import Path

import afx_compile
import afx_music_bank


def write_pair(path: Path, raw: bytes, checkpoints: bool = False) -> None:
    samples = [{"raw": raw}]
    bank, offsets, low, high = afx_compile.build_bank_payload(samples)
    offset = offsets[0]
    setup = struct.pack("<18H", offset >> 16, offset & 0xFFFF, 0, len(raw) // 2 - 1, 31, *([0] * 13))
    stream = bytes((0x14, 0, 0, 0, 0, 0, 0, 0, 0))
    checkpoint = b""
    if checkpoints:
        checkpoint = (struct.pack("<4I", afx_music_bank.CHECKPOINT_MAGIC, 1, 1, 0) +
                      struct.pack("<4I", 0, 0, 0, 1) +
                      struct.pack("<I18H", 0, offset >> 16, offset & 0xFFFF,
                                  0, len(raw) // 2 - 1, 31, *([0] * 13)))
    flow = afx_compile.build_bank_flow(setup, [0], samples, offsets, low, high, b"", stream,
                                       0, 1, 1000)
    path.write_bytes(flow)
    path.with_suffix(".afb").write_bytes(bank)
    seek = afx_compile.build_seek_index(flow, checkpoint)
    if seek: path.with_suffix(".afc").write_bytes(seek)


def address(data: bytes, image_at: int, pair: int = 0) -> int:
    control, low = struct.unpack_from("<HH", data, image_at + pair)
    return (control & 0x7F) << 16 | low


def main() -> None:
    duplicate = struct.pack("<32h", *range(32))
    distinct = struct.pack("<32h", *range(1024, 1056))
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        first, second, third = root / "sequence_1.afx", root / "sequence_2.afx", root / "sequence_3.afx"
        write_pair(first, duplicate, checkpoints=True)
        write_pair(second, duplicate)
        write_pair(third, distinct)
        bank, flows, indices, diagnostics = afx_music_bank.merge_bank_flows([third, first, second])
    compact_duplicate, duplicate_format = afx_music_bank.compact_pcm16(duplicate)
    compact_distinct, distinct_format = afx_music_bank.compact_pcm16(distinct)
    header = afx_music_bank.BANK_HEADER.unpack_from(bank)
    assert header[:2] == (afx_music_bank.AFB_MAGIC, 1)
    assert header[6] == len(bank) and header[5] == len(compact_duplicate) + len(compact_distinct)
    assert bank[header[4]:] == compact_duplicate + compact_distinct
    assert set(flows) == {1, 2, 3}
    assert diagnostics == {"flows": 3, "samples": 2, "input_bank_bytes": len(duplicate) * 2 + len(distinct),
                           "sample_bytes": len(compact_duplicate) + len(compact_distinct),
                           "flow_bytes": sum(map(len, flows.values())), "total_bytes": len(bank)}
    for ident, flow in flows.items():
        fields = afx_music_bank.AFX_HEADER.unpack_from(flow)
        assert fields[1] == 7 and fields[10:12] == header[2:4]
        assert address(flow, fields[4]) == (0 if ident in (1, 2) else len(compact_duplicate))
        assert (struct.unpack_from("<H", flow, fields[4])[0] >> 7) & 3 == \
               (duplicate_format if ident in (1, 2) else distinct_format)
    index = afx_music_bank.AFC_HEADER.unpack_from(indices[1])
    assert index[2] == afx_music_bank.AFX_HEADER.unpack_from(flows[1])[8]
    assert address(indices[1], index[5] + 36) == 0
    print("final AFB+AFX shared-bank merge checks passed")


if __name__ == "__main__":
    main()
