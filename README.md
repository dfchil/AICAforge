# AICAforge

Standalone native/offline audio authoring for Dreamcast's Yamaha AICA.
MIDI/SF2/PCM, N64 CSeq/ALBank and Sega MultiPCM VGM/VGZ become AFB sample banks,
AFX register flows and optional AFC/AFV/AFI sidecars. Bank merging, sample
conversion, optimization and performance profiles are included.

Playback, DSP, firmware, validation and the persistent tuner belong to
[AICAflow](https://github.com/dfchil/AICAflow).

## Build and check

```sh
git clone https://github.com/dfchil/AICAforge.git
cd AICAforge
make -j8
python3 -m pip install mido sf2utils numpy
make check
```

Native builds need Clang, libm and zlib headers (on Debian/Ubuntu:
`apt install clang zlib1g-dev`). Python packages are needed only for tests and
research utilities. No KOS, enDjinn, submodules or sibling runtime checkout is
required. The six executables are written to `build/`.

See [command recipes](src/README.md), [authoring workflow](docs/authoring.md)
and [binary contract](dependencies/aicaflow-format/docs/assets.md).

## Compatibility boundary

`dependencies/aicaflow-format/` is a small, versioned copy of AICAflow's
canonical public format layer, not runtime internals. Its `VERSION` records
the exact upstream tag, commit and format API. File versions remain AFB 1,
AFX 7, AFC 1 and AFI 1. Repository release versions are independent.

```sh
make update-aicaflow-format VERSION=aicaforge-extraction-v1
make check
make -C /path/to/AICAflow compatibility-check AICAFORGE_BIN="$PWD/build"
```

Only maintainers update the vendored layer. Review its diff and run both test
suites before committing. The updater refuses to overwrite local dependency
changes. CI tests current AICAforge against current AICAflow and its frozen
pre-split assets, including format invariants, deterministic output, bank
binding and stale-checkpoint rejection.

To build AICAflow examples with these tools:
`make -C /path/to/AICAflow examples AICAFORGE_BIN="$PWD/build"`.

## History and transition

Authoring history was extracted using git-filter-repo 2.47.0 from AICAflow's
`aicaforge-extraction-v1` tag. Authors, dates and messages are retained,
including paths preceding the old `tools/author/` layout. See
`git log --follow -- src/afx_compile_c.c` and
[the reproducible migration procedure](https://github.com/dfchil/AICAflow/blob/main/docs/repository-split.md).

The `repo-split-v1` tag in each repository marks the initial compatible pair.
AICAflow temporarily retains a deprecated authoring copy so existing builds
can migrate; new authoring development belongs here. Removal is a later,
announced transition release, not part of this extraction.
