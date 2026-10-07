# Building

Linux builds are verified. Windows and macOS support is still in development.
**The game is not playable yet**; the default build provides the launcher and
local disc checks.

On a Mac, follow the dedicated [macOS setup and build instructions](BUILDING_MACOS.md).

## Requirements

- Git, CMake 3.25+, Ninja, and Python 3.10+.
- A C/C++20 compiler and Rust/Cargo 1.85+.
- SDL's platform development dependencies. On Linux, install `pkg-config` and
  the X11 and/or Wayland development packages listed in
  [SDL's Linux notes](../extern/sdl/docs/README-linux.md).

The first build may download locked Rust dependencies. Building and running the
default tests does not require game data.

## Build and run

From the repository root:

```sh
git -c submodule.recurse=false submodule update --init --checkout -- extern/mscharged-decomp extern/nod extern/corrosion extern/sdl extern/imgui
cmake --workflow --preset release
./build/release/mscharged
```

The workflow configures, prepares sources, builds, and runs tests. Patches apply
to generated copies under `build/`, keeping submodules unchanged. See the
[patch workflow](../patches/README.md) when developing source changes.

Release enables optimizations. For a Debug build:

```sh
cmake --workflow --preset debug
./build/debug/mscharged
```

For incremental builds and tests, use `cmake --build --preset release` and
`ctest --preset release` (or replace `release` with `debug`). Run the executable
with `--help` for available options.

## Configure your disc

Supply your own Mario Strikers Charged ISO or RVZ. You can store it in `game/`,
which is excluded from Git. Select it in the launcher and save your settings,
or copy `mscharged.ini.example` to `mscharged.ini` and set:

```ini
[game]
disc = game/R4QE01.rvz
```

Your personal `mscharged.ini` is also excluded from Git. Relative disc paths
resolve from the INI's directory. To use another configuration:

```sh
./build/release/mscharged --config /path/to/mscharged.ini
```

## Run-only launch settings

`--disc FILE` (or `--disk FILE`), `--window`, `--fullscreen`, and
`--size WIDTHxHEIGHT` override the loaded INI for the current run. They do not
save settings. Command-line disc paths use the working directory; INI disc
paths use the INI directory. Later options win.
Options with values also accept `--disc=FILE`, `--size=1280x720`, etc. Shared
options can appear before or after the experimental mode flag. Quote paths
containing spaces.

```sh
./build/release/mscharged --disc ./game/R4QE01.rvz --window --size 1280x720
```

This still opens the launcher. Select an experimental mode explicitly; these
settings do not select or complete game startup.

## Experimental builds

See [runtime status and commands](RUNTIME.md) for the original Boot and Credits
scene tests and startup checks. These use additional dependencies and currently
support USA `R4QE01` revision 1 game data.
