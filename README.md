# Mario Strikers Charged — Native Port

`mscharged-port` aims to bring **Mario Strikers Charged** to Windows, Linux,
macOS, and additional platforms as a native application, built on
[mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp).

The goal is to preserve the original game's gameplay, physics, timing, and
presentation while adapting it to modern hardware.

## Status

An initial native development bootstrap builds a small subset of the decompiled
utilities. Pinned source dependencies, an ordered patch preparation step, and
focused checks are in place. There is no playable game build yet; Aurora's
runtime and the UI libraries have not been integrated into an executable.

**The decompilation is not yet complete or fully linked.** Some functions and
translation units may be missing, unfinished, or unsuitable for native
compilation. The bootstrap uses an explicit subset of available source and does
not stand in for a complete game link. The first 100% decompilation and 100%
link release will provide a later baseline.

See [build instructions](docs/BUILDING.md) to build and run the bootstrap.
The platforms and features below remain development goals.

## Goals

- Run natively on Windows, Linux, and macOS, with room for additional platforms.
- Preserve the original game experience and validate behavior against the Wii
  version.
- Adapt Wii controls for modern controllers and keyboard input, with
  configurable bindings.
- Support modern resolutions and display options.
- Deliver efficient performance on desktops and handheld PCs.
- Keep dependencies connected to their upstream projects, with clear attribution
  and reviewable updates.

## Development plan

1. Expand the native source subset and patch series as the decompilation
   progresses, and integrate the pinned Aurora runtime and selected libraries.
2. Bring up the runtime, including game data access, graphics, input, and audio.
3. Reach a playable match and verify gameplay behavior against the original.
4. Expand game coverage, validate the initial platform targets, and add modern
   display and control options.

Game installation instructions will follow when a playable build is available.

## Source projects and dependencies

The port's source foundations are:

- [mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp) by
  Yannick Suter and its contributors, which reconstructs the original Wii
  game's source code.
- [Aurora](https://github.com/encounter/aurora) by Luke Street and its
  contributors, a source-level GameCube and Wii compatibility layer.

These projects and the initial graphics, input, utility, and UI libraries are
referenced through Git submodules. Each submodule is pinned to a specific
commit. New commits in an upstream repository do not change the source used by
an existing port checkout. See the [dependency inventory](extern/README.md) for
source URLs, revisions, purposes, and integration status.

Aurora also manages dependencies through its own build system. Selecting these
local sources as build providers is part of runtime integration. Optional UI
sources are checked out for development; their presence does not imply that
every library will ship. Every dependency retains its applicable license and
notices.

## Source preparation and build

The build keeps upstream submodules unchanged and applies the port's adaptations
to generated source copies. [The preparation tool](tools/prepare_sources.py)
runs during CMake configuration. A check before compilation rejects changed
pins, tracked upstream edits, stale inputs, or modified generated source.

The layout is:

```text
extern/mscharged-decomp/      Upstream decompilation submodule
extern/aurora/                Upstream Aurora submodule
patches/mscharged-decomp/     Ordered patches to the decompiled source
patches/<dependency>/         Additional patch series, when needed
src/                         Code and platform adapters developed for this port
tools/                       Source preparation and build tooling
build/                       Generated patched sources and build outputs
```

The build sequence is:

1. Verify that each submodule is at its recorded commit and has no tracked
   modifications.
2. Export the committed source into a clean, generated build directory,
   including any required nested dependencies at their recorded revisions.
3. Apply each dependency's patch series in an explicit order. Stop if a patch
   fails; do not compile a partially patched tree.
4. Configure and compile the prepared sources together with this repository's
   port code, then run the relevant checks.

Every change to upstream source is recorded as a versioned patch. Each patch
explains its purpose, and each series identifies its upstream base. New platform
adapters and other independent port code live in this repository. See
[patch development](patches/README.md) for application order, validation, and
exporting edits from a generated tree.

Preparation runs before source configuration when the recorded revisions, patch
contents or order, or preparation tooling change. Generated sources stay out of
Git and can be regenerated from those inputs. A normal build uses the recorded
dependencies without updating them to upstream branch tips.

## Decompilation releases and dependency updates

Decompilation releases, starting with the planned **1.0** release, will provide
named baselines for the port. The submodule will record the exact commit behind
the selected release. Until that release exists, development may use an
explicitly selected development commit under the same pinning policy.

The decompilation can continue to fix matching issues, improve documentation,
and clean up code while the port keeps using its tested baseline. Adopting a
newer release will be a separate change: update the submodule pin, review and
refresh the patches, build the port, and validate behavior before accepting the
upgrade. A release label does not guarantee that existing patches will apply
or that native behavior is unchanged.

Published release tags should remain fixed; later corrections belong in new
releases. The port will have its own version numbers and record the decompilation
release and commit, Aurora revision, and patch set used for each release.
The same deliberate update policy applies to other dependencies, including
those fetched by upstream build systems.

## Game data

Players will need game data from their own legally obtained copy of Mario
Strikers Charged. Game assets are not included in this repository and will not
be distributed with the port. Supported disc versions and setup instructions
will be documented when game data loading is implemented.

## Contributing

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for the
project's development priorities, dependency policy, and contribution process.

## License

The project's original port code, tools, and documentation are dedicated to the
public domain under [CC0 1.0 Universal](LICENSE), to the extent their authors
hold the relevant rights. Third-party material retains its own terms; see
[license scope and third-party notices](LICENSES/README.md).

This dedication does not grant rights to the original game or its assets.
Mario Strikers Charged was developed by Next Level Games and published by
Nintendo. This is an unofficial project, unaffiliated with and not endorsed
by either company.
