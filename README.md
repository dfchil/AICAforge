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

See the [documentation index](docs/README.md) for guides, command recipes,
format references and dependency updates.

Code is MIT unless otherwise noted; see [third-party code attribution](ASSET_LICENSES.md).
