# Contributing

Contributions to `mscharged-port` are welcome. The initial native bootstrap and
source preparation workflow are available. The first priorities are extending
the compilable source subset, integrating the runtime, and building a faithful
playable foundation.

The decompilation is incomplete and not fully linked. Add translation units
explicitly as their dependencies become available. Document missing functions
and unsupported paths; do not add silent success stubs to make a full link
appear complete.

## Before starting

Read the [project goals and current status](README.md). Open an issue before
starting a substantial feature, platform backend, or dependency change so that
the approach can be discussed and duplicate work avoided. Small documentation
corrections can go directly into a pull request.

Keep changes focused. Describe the problem being solved and how the change helps
the port. Preserve the original game's behavior unless a change is an explicit,
documented enhancement.

## Source and dependency changes

- Reference directly used source dependencies through Git submodules pointing
  to their original upstream repositories, pinned to specific commits.
- Prefer a published decompilation release as the baseline once one is
  available. Record its exact commit; ordinary builds must not follow a moving
  branch or run `git submodule update --remote`.
- Explain why a dependency is needed. For updates, identify the old and new
  revisions and describe the relevant changes and validation.
- Keep platform adapters and port code identifiable within this project.
  Record necessary upstream source changes as documented patches, with the
  reason for each change and the revision it applies to.
- Keep each patch focused on one purpose and list patches in an explicit
  series order. Avoid unrelated formatting changes that make patches harder
  to review and update.
- Keep upstream checkouts clean. Apply patches to generated source copies in
  the build directory before configuring or compiling. Source preparation must
  stop on a failed patch and must not use a partially prepared or stale tree.
- Export changes made while developing in generated sources back into the
  patch series before regenerating those sources. A fresh checkout must be
  able to reproduce the complete prepared source from recorded inputs.
- Preserve upstream copyright notices, license texts, and authorship.
  Update [LICENSES/README.md](LICENSES/README.md) when adding a dependency.
- Consider whether a general fix belongs in the decompilation or library
  upstream, and keep it suitable for contributing there.

Account for dependencies managed by upstream build systems, including Aurora's,
when changing the build. Avoid introducing duplicate or conflicting versions.

Treat a dependency upgrade as a focused change containing the new pin, any
patch updates or removals, and validation results. Patches that apply cleanly
still need compilation and behavior checks. When an upstream fix replaces a
local workaround, remove the workaround during the upgrade. Keep the previous
pin and patch set reproducible through the port's Git history.

## Validation

Follow [the build instructions](docs/BUILDING.md) and run `ctest --test-dir build
--output-on-failure`. The current checks cover the selected utility code and
source preparation. They do not establish game completeness or gameplay parity.
See [patch development](patches/README.md) when changing upstream adaptations.

For implementation changes, report the platform, architecture, compiler, and
checks used to validate the result. For gameplay changes, describe how the
behavior was compared with the original game. State which relevant platforms
or behaviors remain untested.

## Pull requests

Use a clear title and include:

- The problem or goal and the resulting behavior.
- The implementation choices a reviewer needs to understand.
- Validation performed and any known limitations.
- Related issues or upstream changes, where applicable.

Keep unrelated cleanup in a separate change. Add or update documentation when
the build, controls, dependency setup, or user-facing behavior changes.

## Game data

Use your own legally obtained game copy for local development. Keep disc images,
extracted game files, and other game data in the ignored `game-data/` or `orig/`
directories. Do not include game binaries or assets in commits, pull request
attachments, or releases.

## Licensing

By submitting original code, tools, or documentation for inclusion, you agree
to dedicate your contribution under [CC0 1.0 Universal](LICENSE), to the extent
you hold the relevant rights. Identify third-party material and preserve its
applicable license and notices. The project's dedication does not change the
terms of that material or grant rights to the original game.
