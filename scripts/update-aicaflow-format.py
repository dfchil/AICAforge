#!/usr/bin/env python3
"""Explicit, reviewable update of the one vendored public format dependency."""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TARGET = ROOT / "dependencies/aicaflow-format"
UPSTREAM = "https://github.com/dfchil/AICAflow.git"


def git(*args, cwd=ROOT, **kwargs):
    return subprocess.run(["git", *args], cwd=cwd, check=True, **kwargs)


def main():
    if len(sys.argv) != 2 or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._/-]*", sys.argv[1]):
        raise SystemExit("usage: update-aicaflow-format.py UPSTREAM_TAG")
    tag = sys.argv[1]
    dirty = git("status", "--porcelain", "--untracked-files=all", "--",
                str(TARGET), capture_output=True, text=True).stdout
    if dirty or TARGET.is_symlink():
        raise SystemExit("Commit or discard local format changes before updating")
    with tempfile.TemporaryDirectory(prefix=".format-update-", dir=TARGET.parent) as directory:
        stage = Path(directory)
        repo, incoming = stage / "upstream", stage / "format"
        git("init", "-q", str(repo))
        git("fetch", "-q", "--depth=1", UPSTREAM, f"refs/tags/{tag}", cwd=repo)
        commit = git("rev-parse", "FETCH_HEAD^{commit}", cwd=repo,
                     capture_output=True, text=True).stdout.strip()
        archive = stage / "format.tar"
        with archive.open("wb") as output:
            git("archive", f"{commit}:format", cwd=repo, stdout=output)
        incoming.mkdir()
        subprocess.run(["tar", "-xf", str(archive), "-C", str(incoming)], check=True)
        header = (incoming / "include/aicaflow/format.h").read_text()
        api = re.search(r"^#define AFX_FORMAT_API_VERSION (\d+)$", header, re.MULTILINE)
        if not api or not (incoming / "src/codec.c").is_file():
            raise SystemExit("Upstream tag does not contain the public format contract")
        (incoming / "VERSION").write_text(
            f"upstream={UPSTREAM}\naicaflow-ref={tag}\naicaflow-commit={commit}\n"
            f"format-api={api[1]}\n")
        backup = stage / "previous"
        TARGET.rename(backup)
        try:
            incoming.rename(TARGET)
        except OSError:
            backup.rename(TARGET)
            raise
    print(f"Updated format to {tag} ({commit}); review the diff and run compatibility tests")


if __name__ == "__main__":
    main()
