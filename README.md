# Mario Strikers Charged — Native Port

[![Build stable](https://github.com/yannicksuter/mscharged-port/actions/workflows/build.yml/badge.svg?branch=stable)](https://github.com/yannicksuter/mscharged-port/actions/workflows/build.yml?query=branch%3Astable)
[![Development version: 0.0.1-dev](https://img.shields.io/badge/version-0.0.1--dev-blue)](CMakeLists.txt)

A native C/C++ port of **Mario Strikers Charged** for modern systems, based on
[mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp) and
[Aurora](https://github.com/encounter/aurora). It compiles reconstructed game
source into native machine code; it is not a recompilation of the PowerPC binary.

## Status

**Work in progress.** Native platform integration is ongoing.
Experimental startup and menus run on Linux; a complete playable game and
Windows/macOS game support remain in development.

## Checkout, build, run

Install the [build prerequisites](docs/BUILDING.md#requirements) first; Mac users
can follow [macOS tool setup](docs/BUILDING_MACOS.md#install-tools).
For a **fresh checkout**, run these three lines from the parent directory:

The Release build includes the incomplete original-source frontend runtime.
Supply your own USA `R4QE01` revision 1 ISO/RVZ at the path below. Linux startup
and menus have been tested; macOS runtime validation is still in progress.

```sh
git clone --branch main --no-recurse-submodules https://github.com/yannicksuter/mscharged-port.git && cd mscharged-port
python3 tools/setup_dependencies.py && CMAKE_BUILD_PARALLEL_LEVEL=3 cmake --workflow --preset release --fresh
./build/release/mscharged --disc ./game/R4QE01.rvz --window
```

`main` is the development branch; `stable` is for reviewed snapshots promoted
through pull requests. The [GitHub build workflow](docs/BUILDING_GITHUB.md)
checks promotions and provides binaries after merging. The helper initializes
only the pinned dependencies required by your platform. See the [build guide](docs/BUILDING.md)
for updates and the smaller launcher build, and [runtime instructions](docs/RUNTIME.md)
for separate diagnostics.

## Game data

Supply an **ISO or RVZ from your own copy**. Game data is not included. Files in
`game/` and your personal `mscharged.ini` are excluded from Git.
See [disc setup](docs/BUILDING.md#configure-your-disc).

## Controls

The keyboard and mouse act as a Wii Remote with a Nunchuk; the original game
decides what each input does. Keep the game window focused.

| Wii input | Keyboard / mouse |
| --- | --- |
| Pointer | Mouse |
| A | Enter, Space, or left click |
| B | Esc, Backspace, or right click |
| D-pad | Arrow keys |
| 1 / 2 | Z / X |
| + (Plus) / − (Minus) | Tab / - |
| HOME | Home |
| Nunchuk stick | W A S D |
| Nunchuk C / Z | C / V |
| Shake Wii Remote (hit an opponent) | E |
| Shake Nunchuk (switch items) | Q |

A real **Wii Remote with a Nunchuk** also works (experimental, Linux), with the
mouse as its pointer; see [Wii Remote setup](docs/RUNTIME.md#wii-remote).

Press **P** to save the presented frame as `screenshots/screenshot_<timestamp>.png`
in the directory the port was started from.

## Porting approach

Original source plus reviewed patches becomes the compiled game:

1. **Pin.** The decomp and every dependency stay clean submodules at reviewed
   commits; upstream changes enter only through explicit updates.
2. **Patch.** The build exports the decomp's complete `include/`, `libs/`, and
   `src/` trees and applies the ordered [patch series](patches/README.md):
   compiler compatibility, native replacements for Wii hardware and platform
   services, and data/ABI adaptation.
3. **Format.** clang-format 16+ gives the patched tree one readable layout
   ([definition](tools/formatting/prepared-sources.clang-format)). It changes
   whitespace only and is verified token by token; files whose program depends
   on their layout (`__LINE__`, stringified macro arguments) stay as patched.
4. **Compile.** Original game code controls the game flow from `main(...)`;
   native adapters replace Wii hardware services while preserving retail behavior.

Steps 1–3 run during CMake configuration. To produce the source without
configuring or building:

```sh
git submodule update --init extern/mscharged-decomp
python3 tools/prepare_sources.py --build-dir build/release
```

This writes `build/release/prepared/mscharged-decomp/patched/` (pinned decomp
plus patches, byte-exact; patches are developed here) and `source/` (the
formatted tree that is compiled). A later `cmake --preset release` reuses it.
Without clang-format, `source/` is an unformatted copy.

See the short [port strategy](docs/PORTING.md), [patch workflow](patches/README.md),
and [contributing guide](CONTRIBUTING.md).

## License

Original port code, tools, and documentation use [CC0 1.0 Universal](LICENSE).
Third-party material retains its own terms; see
[license scope and notices](LICENSES/README.md).

Mario Strikers Charged was developed by Next Level Games and published by
Nintendo. This is an unofficial project, unaffiliated with either company.
