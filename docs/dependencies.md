# Dependencies

[Documentation](README.md)

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
