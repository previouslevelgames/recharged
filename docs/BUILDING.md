# Building

Release includes the incomplete original-source frontend runtime. Linux startup
and menus have been tested; macOS runtime validation is in progress. Windows
runtime support remains in development. **A complete playable game is not available.**

On a Mac, follow the [macOS tool setup](BUILDING_MACOS.md#install-tools) first.

## Requirements

- Git, CMake 3.25+, Ninja, and Python 3.10+.
- A C/C++20 compiler and Rust/Cargo 1.85+.
- Make and Tcl 8.6+ for the pinned SQLite generator.
- Optional: clang-format 16+ formats the prepared game sources for reading. The
  build is otherwise identical; without it CMake prints a warning.
- On Linux, a Vulkan driver/loader, `pkg-config`, and the X11/Wayland development
  packages described in [SDL's Linux notes](../extern/sdl/docs/README-linux.md).

The first build may download locked Rust dependencies. Dependencies are built
from their recorded Git revisions.

## Build and run

From the parent directory, with your own USA `R4QE01` revision 1 ISO/RVZ ready:

```sh
git clone --branch main --no-recurse-submodules https://github.com/yannicksuter/mscharged-port.git && cd mscharged-port
python3 tools/setup_dependencies.py && CMAKE_BUILD_PARALLEL_LEVEL=3 cmake --workflow --preset release --fresh
./build/release/mscharged --disc /path/to/R4QE01.rvz --window
```

Quote disc paths containing spaces. The helper initializes only required pins
and stops if a dependency has tracked edits; it does not discard or stash them.
The workflow prepares source copies and builds `mscharged`. Other diagnostics
and tests are separate. To prepare the patched game source alone, see the
[porting approach](../README.md#porting-approach) and the [patch workflow](../patches/README.md).
`-DMSCHARGED_FORMAT_PREPARED_SOURCES=ON` requires formatting; `OFF` skips it.

For an existing checkout:

```sh
git fetch origin
git switch main
git pull --ff-only origin main
python3 tools/setup_dependencies.py
cmake --workflow --preset release --fresh
```

After code changes, use `cmake --build --preset release`. If presets or build
options changed, run `cmake --preset release` first to refresh the configuration.
Run `mscharged --help` for available options.

## Smaller builds and tests

`launcher` and `debug` build the launcher and foundation tools without the game
runtime. Their workflows also build and run the default tests:

```sh
python3 tools/setup_dependencies.py --launcher
cmake --workflow --preset launcher
./build/launcher/mscharged --launcher
```

Use `debug` instead for a Debug build. Runtime diagnostics have independent
[experimental presets](RUNTIME.md); the game-only Release workflow has no test
preset.

## Configure your disc

Your disc and personal `mscharged.ini` stay out of Git. Store your own image in
`game/` if desired. `--disc FILE` selects it for one run and enters the included
original runtime. Add `--launcher` to open settings instead.

For saved configuration, copy `mscharged.ini.example` to `mscharged.ini`:

```ini
[game]
disc = game/R4QE01.rvz
```

Relative INI paths resolve from the INI directory. Use `--config FILE` to select
another configuration.

## Run-only launch settings

`--disc` (also `--disk`), `--window`, `--fullscreen`, and `--size WIDTHxHEIGHT`
override settings for the current run without saving them. Command-line disc
paths resolve from the working directory. Values also accept `--disc=FILE`.

```sh
./build/release/mscharged --disc ./game/R4QE01.rvz --window --size 1280x720
```

Explicit experimental modes retain their selected mode. Launcher-only builds
report that the game runtime is unavailable when asked to start it directly.
