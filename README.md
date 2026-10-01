# Mario Strikers Charged — Native Port

`mscharged-port` aims to bring **Mario Strikers Charged** to Windows, Linux,
macOS, and additional platforms as a native application, built on
[mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp).

The goal is to preserve the original game's gameplay, physics, timing, and
presentation while adapting it to modern hardware.

## Status

An initial native development bootstrap builds a small subset of the decompiled
utilities and reads Wii ISO/RVZ images through nod. Pinned source dependencies,
an ordered patch preparation step, and focused checks are in place. There is no
playable game build yet; Aurora's runtime and the UI libraries have not been
integrated into an executable.

**The decompilation is not yet complete or fully linked.** Some functions and
translation units may be missing, unfinished, or unsuitable for native
compilation. The bootstrap uses an explicit subset of available source and does
not stand in for a complete game link. The first 100% decompilation and 100%
link release will provide a later baseline.

The platforms and features below remain development goals.

## Build the first development version

The current port version is **0.0.1-dev**. With Git, Python 3.10+, CMake 3.25+,
Ninja, C/C++17 compilers, and Rust/Cargo 1.85+ installed, run from the repository root:

```sh
git submodule update --init --checkout extern/mscharged-decomp extern/nod extern/corrosion
cmake --workflow --preset release
./build/release/mscharged-bootstrap --version
./build/release/mscharged-bootstrap --self-test
```

The workflow prepares the patched sources, compiles, and runs the tests. This
build runs native utility checks and can inspect a configured disc image; game
startup and rendering are still pending. The first build downloads Rust crates
at the versions recorded in nod's lockfile.
The executable includes the port's Git revision in its version, with `.dirty`
appended when the checkout has uncommitted changes. Port versions are independent
of decompilation releases.

See [build instructions](docs/BUILDING.md) for Windows, Debug builds, and version
details. Linux builds have been verified; Windows and macOS remain unvalidated.

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

Place an **ISO or RVZ** from your own copy of Mario Strikers Charged in `game/`.
Only `game/.gitkeep` is intended for Git; disc images and other local contents
are ignored. Images elsewhere on your system can also be used without copying.

Copy `mscharged.ini.example` to `mscharged.ini` and set its `[game] disc` path:

```ini
[game]
disc = game/R4QE01.rvz
```

The personalized `mscharged.ini` is ignored by Git. Paths are relative to the
INI file, and ISO/RVZ images are read directly without conversion or extraction.
Run from the repository root:

```sh
./build/release/mscharged-bootstrap
```

This checks the disc identity and opens its game data partition. The current
source baseline is **USA `R4QE01`, revision 1**; other Charged regions/revisions
can be inspected but their asset compatibility is unverified. This check does
not verify every disc block or start the game. See [disc setup options](docs/BUILDING.md#configure-your-disc).

Disc images and extracted game data are not included or distributed with this
project. The selected [launcher header artwork](assets/launcher/README.md) has
separate source attribution; the graphical launcher is still pending.

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
