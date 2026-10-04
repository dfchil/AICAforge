# Changelog

## 0.1.0-rc1 (unreleased)

- Standalone native MIDI/SF2/PCM, N64 CSeq/ALBank and MultiPCM VGM/VGZ tools.
- Shared-bank building and merging, performance profiles and visualisation output.
- Pinned AICAflow SDK for format definitions, validation and compatibility tests.
- Task-based documentation for authoring, formats, resource budgets and tests.

### Migration from AICAflow authoring

Run `make dependencies`, then `make`; executables are written to `build/`.
Use `make check` for authoring tests and `make compatibility-check` for SDK
integration. See [Testing](docs/testing.md) for test dependencies.

The SDK dependency is non-recursive. `make update-dependencies` explicitly
updates it; ordinary builds retain the recorded revision. Runtime output
versions are AFB 1, AFX 7, AFC 1 and AFI 1.

No release tag or binary distribution has been published for this candidate.
