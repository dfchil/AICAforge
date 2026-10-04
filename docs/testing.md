# Asset tests and validation

Run from the AICAforge root:

```sh
make dependencies
python3 -m pip install mido sf2utils numpy
make check
make compatibility-check
```

Host tests need Clang with AddressSanitizer/UndefinedBehaviorSanitizer,
zlib and Python 3. They do not need KOS or Dreamcast hardware.

`make check` covers compiler/codec behavior, importers, profiles, bank merging
and repeatable output. CLI fixtures compare repeated AFB/AFX/AFC/AFV builds
byte-for-byte. `make compatibility-check` also runs the pinned SDK's validator,
simulated loader and frozen-asset tests, including bank/checkpoint binding checks.

## Validate a generated flow

```sh
make -C dependencies/AICAflow/driver build/afx_validate
dependencies/AICAflow/driver/build/afx_validate output/song.afx
```

The validator checks AFX structure and command constraints. It does not load
the sibling AFB/AFC, test the application's full memory budget or verify sound.
Use compatibility tests for loader behavior and the target player for audition.

## Troubleshooting

- **Bank build fails:** check input paths relative to the map, SF2 bank/program
  selections and conversion options. See [Authoring](authoring.md).
- **Profile rejected:** inspect the new base AFX inventory and recreate the AFP
  binding. Apply it with the AFC from that same build.
- **Bank/checkpoint mismatch:** deploy the matching generated AFB/AFX/AFC set;
  do not mix files from separate builds or from before and after a bank merge.
- **Memory, voice or work limit exceeded:** review the
  [resource budget](authoring.md#output-and-resource-budgets).
- **Playback sounds wrong:** audition the unprofiled output, then the derived
  output; compare source selection, gain, pitch, loops and release behavior.

Game pack checks require the application's own extracted inputs; see
[AFSFX verification](specs/afsfx.md#verify). Playback, seek, DSP and lifetime
checks run on hardware; see the SDK's [hardware testing guide](../dependencies/AICAflow/docs/testing.md).
