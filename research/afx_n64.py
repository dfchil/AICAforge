#!/usr/bin/env python3
"""Read supported Nintendo 64 music formats into AICAflow authoring data.

``cseq`` writes a complete AFB/AFX/AFC bundle from libaudio CSeq + ALBank.
``audioseq`` writes the equivalent OoT trace; its samples are resolved and
packed by the OoT bank build because AudioSeq references the game's extracted
Audiobank/Audiotable rather than an ALBank pair.
"""
from __future__ import annotations

import sys

def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in {"-h", "--help"}:
        print("usage: afx_n64.py {cseq|audioseq} ...", file=sys.stderr)
        return 2
    command = argv.pop(0)
    if command == "cseq":
        import afx_n64_cseq
        return afx_n64_cseq.main(argv)
    if command == "audioseq":
        import afx_n64_audioseq
        return afx_n64_audioseq.main(argv)
    print(f"afx-n64: unsupported source format {command!r}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
