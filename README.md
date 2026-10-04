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
make dependencies
make -j8
python3 -m pip install mido sf2utils numpy
make check
```

Native builds need Clang, libm and zlib headers (on Debian/Ubuntu:
`apt install clang zlib1g-dev`). Python packages are needed only for tests and
research utilities. The pinned AICAflow submodule supplies the SDK; no KOS,
enDjinn or sibling checkout is required. The six executables are written to `build/`.

See [command recipes](src/README.md), [authoring workflow](docs/authoring.md)
and [binary contract](dependencies/AICAflow/format/docs/assets.md).

## Compatibility boundary

AICAforge depends on the driver repository at `dependencies/AICAflow/`,
pinned by Git's submodule commit. It compiles the public format layer directly
from `dependencies/AICAflow/format/`; there is no separate vendored copy.
The driver's validator, simulated loader tests and frozen fixtures are used
by `make compatibility-check`. Authoring still does not include private
firmware/IPC headers. File versions remain AFB 1, AFX 7, AFC 1 and AFI 1.

`make dependencies` initializes **only this SDK**, never its enDjinn or
AICAforge example dependencies. Use this target, not recursive submodule
checkout. The build graph is driver → authoring → examples, not a recursive
build of both repositories.

Maintainers update a pin using ordinary Git (start with a clean submodule):

```sh
make update-dependencies
make check compatibility-check
git add dependencies/AICAflow
git commit -m "Update AICAflow SDK"
```

The update target explicitly follows AICAflow's `main`; normal `make` never
fetches or updates dependencies. `make dependencies` restores the recorded
revision. Review and commit a successful update's gitlink change. For a
specific release, use `git -C dependencies/AICAflow checkout --detach <tag>`.

The old format updater and `VERSION` file are replaced by the submodule pin.
CI checks both the pinned SDK and current AICAflow.

To build AICAflow examples with these tools:
`make -C /path/to/AICAflow examples AICAFORGE_BIN="$PWD/build"`.

## History and transition

Authoring history was extracted using git-filter-repo 2.47.0 from AICAflow's
`aicaforge-extraction-v1` tag. Authors, dates and messages are retained,
including paths preceding the old `tools/author/` layout. See
`git log --follow -- src/afx_compile_c.c` and
[the reproducible migration procedure](https://github.com/dfchil/AICAflow/blob/main/docs/repository-split.md).

The `repo-split-v1` tag in each repository marks the initial compatible pair.
That initial release retained an AICAflow authoring copy. The follow-up cleanup
removes it at the owner's request: authoring code, tests and documentation now
live only here. AICAflow examples consume the executables from this checkout.

Code is MIT unless otherwise noted; see [third-party code attribution](ASSET_LICENSES.md).
