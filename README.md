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

See [command recipes](src/README.md), [authoring workflow](docs/authoring.md),
[authoring formats and AFV](docs/specs/authoring-formats.md), and the
[runtime binary contract](dependencies/AICAflow/driver/format/docs/assets.md).
See [asset testing](docs/testing.md) for validation and troubleshooting.

## Driver dependency

`dependencies/AICAflow/` is a pinned Git submodule. The compiler uses
`driver/format/`; `make compatibility-check` runs the driver's validator,
loader tests and frozen fixtures. CI tests the pinned SDK and current AICAflow.

`make dependencies` restores the recorded SDK revision without initializing
its nested dependencies. Use this target instead of recursive submodule checkout.

To update the SDK, start with a clean submodule:

```sh
make update-dependencies
make check compatibility-check
git add dependencies/AICAflow
git commit -m "Update AICAflow SDK"
```

The update target follows AICAflow's `main`. Normal builds do not fetch or
update dependencies. For a specific release, use
`git -C dependencies/AICAflow checkout --detach <tag>`, test and commit the pin.

To build AICAflow examples with these tools:
`make -C /path/to/AICAflow examples AICAFORGE_BIN="$PWD/build"`.

Code is MIT unless otherwise noted; see [third-party code attribution](ASSET_LICENSES.md).
