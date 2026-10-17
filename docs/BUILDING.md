# Building from source

You only need this to build Recharged yourself; to play, the
[ready-made packages](INSTALL.md) are simpler. The build compiles the game from
the pinned decompilation, so it takes a while the first time. You still need
your own disc image to play (USA `R4QE01` revision 1, or Europe `R4QP01`
revision 1 or 2): no game files are included. How the build turns the decomp into a program is explained in the
[architecture](ARCHITECTURE.md) document.

## Requirements

- Git, CMake 3.25+, Ninja and Python 3.10+
- A C/C++20 compiler (GCC 14+ or Clang) and Rust/Cargo 1.85+
- Make and Tcl 8.6+ (they generate SQLite from its pinned source)
- Optional: clang-format 16+, which formats the prepared game sources for
  reading. The program is the same without it; CMake prints a warning.
- Linux: a Vulkan driver and loader, `pkg-config`, and the X11/Wayland
  development packages listed in
  [SDL's Linux notes](https://github.com/libsdl-org/SDL/blob/main/docs/README-linux.md).

The first build downloads the locked Rust dependencies. Every other dependency
is built from its pinned submodule.

### macOS tools

Install Apple's Command Line Tools (`xcode-select --install`), then
[Homebrew](https://docs.brew.sh/Installation) and the remaining tools:

```sh
brew install cmake ninja python rust tcl-tk clang-format
export PATH="$(brew --prefix tcl-tk)/bin:$PATH"
```

The system Tcl of older macOS versions is too old. If you already use rustup,
keep it instead of installing Rust twice. Build in a native terminal, not
under Rosetta; Intel Macs build the same way.

## Build and run

With your disc image ready, from the folder that should contain the checkout:

```sh
git clone --no-recurse-submodules https://github.com/previouslevelgames/recharged.git && cd recharged
python3 tools/setup_dependencies.py && CMAKE_BUILD_PARALLEL_LEVEL=3 cmake --workflow --preset release --fresh
./build/release/recharged
```

This builds `stable`, the released state. Add `--branch main` to the clone for
the current development state. Use a Git clone, not GitHub's ZIP download.
`setup_dependencies.py` initializes only the submodules the build needs, at
their recorded commits; it stops, without changing anything, if a dependency
has local edits. Raise `CMAKE_BUILD_PARALLEL_LEVEL` if you have the memory for
more parallel jobs.

The build writes the program to `build/release/`: `recharged` starts the
launcher, and `recharged --disc FILE` starts the game directly
([options](SETTINGS.md#command-line-options)).

To update an existing checkout:

```sh
git pull --ff-only
python3 tools/setup_dependencies.py
cmake --workflow --preset release --fresh
```

After changing code, `cmake --build --preset release` is enough; if presets or
build options changed, run `cmake --preset release` first. `--fresh` clears
only the CMake configuration, not sources or settings.

### If configuration fails

Read the `Source preparation failed` or Git message above CMake's summary. A
reported edit in a dependency can be inspected with
`git -C extern/NAME diff HEAD`; save it before retrying, and do not force-reset
dependencies. On macOS, if CMake picked an old system Tcl, pass the Homebrew
one:

```sh
cmake --fresh --preset release -DMSCHARGED_SQLITE_TCLSH="$(brew --prefix tcl-tk)/bin/tclsh"
cmake --build --preset release
```

## Disc and settings

Your disc image and personal `recharged.ini` stay out of Git; the `game/`
folder is ignored except for its placeholder. To keep settings, copy
`recharged.ini.example` to `recharged.ini`:

```ini
[game]
disc = game/R4QE01.rvz
```

Paths in the INI are relative to the INI file; paths on the command line are
relative to the working directory. `--config FILE` selects another INI.

## Windows

Windows packages are cross-compiled on Linux with the pinned
[LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw) toolchain (release
20261006, as in CI) and the Rust target `x86_64-pc-windows-gnullvm`:

```sh
rustup target add x86_64-pc-windows-gnullvm
python3 tools/setup_dependencies.py
python3 tools/setup_windows_graphics.py
cmake --preset release -B build/windows \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/llvm-mingw-x86_64.cmake \
    -DLLVM_MINGW_ROOT=/path/to/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64 \
    -DBUILD_TESTING=OFF
cmake --build build/windows --target mscharged mscharged_original_frontend_module_us \
    mscharged_original_frontend_module_eu
```

The targets keep their old names; they build `recharged.exe` and the game
modules `recharged_game_us.dll` and `recharged_game_eu.dll`.

The targets keep their old names; they build `recharged.exe` and the game
modules `recharged_game_us.dll` and `recharged_game_eu.dll`.

`setup_windows_graphics.py` downloads the checksum-pinned Microsoft DirectX
shader compilers and their notices into `build/windows-graphics`; the build
verifies them offline and the packager includes them.

## Smaller builds and tests

The `launcher` and `debug` presets build the launcher and the foundation tools
without the game, and run the default tests:

```sh
python3 tools/setup_dependencies.py --launcher
cmake --workflow --preset launcher
```

`ctest --preset debug` runs the tests of a debug build. Packages are made with
`tools/package_build.py`.
