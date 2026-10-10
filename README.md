# Mario Strikers Charged — Native Port

[![Build stable](https://github.com/yannicksuter/mscharged-port/actions/workflows/build.yml/badge.svg?branch=stable)](https://github.com/yannicksuter/mscharged-port/actions/workflows/build.yml?query=branch%3Astable)
[![Version 1.0.0](https://img.shields.io/badge/version-1.0.0-blue)](CMakeLists.txt)

Play **Mario Strikers Charged** natively on your PC. The port compiles the
fully reconstructed game source of
[mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp) into native
code, with [Aurora](https://github.com/encounter/aurora) in place of the Wii
hardware. It is not an emulator and not a recompilation of the PowerPC binary:
the original game runs unchanged, modern systems provide the rest.

> [!IMPORTANT]
> Only the **USA version** of the game (**R4QE01**, revision 1) is supported.
> European and Japanese discs need their own complete decompilation before they
> can run glitch free.

## Download and play

You don't need to build anything: ready-made builds for **Windows, Linux and
macOS** are available.

1. Open the [**latest release**](https://github.com/yannicksuter/mscharged-port/releases/latest)
   and download the package for your system:

   | System | Package |
   | --- | --- |
   | Windows 10/11, 64-bit | `mscharged-<version>-windows-x86_64.zip` |
   | Linux, x86_64 | `mscharged-<version>-linux-x86_64.tar.gz` |
   | macOS, Apple Silicon (M1 or newer) | `mscharged-<version>-macos-arm64.tar.gz` |

2. Unpack it into a folder of your choice and keep all its files together.
3. Start **`mscharged.exe`** (Windows) or **`mscharged`** (Linux, macOS).
4. In the launcher, choose your own **USA disc image** (ISO or RVZ, see
   [game data](#game-data)) and press **Play**.

The builds are not signed, so the system asks once on the first start:

- **Windows:** if SmartScreen warns, choose *More info → Run anyway*.
- **macOS:** allow the app under *System Settings → Privacy & Security*, or run
  `xattr -dr com.apple.quarantine <unpacked folder>`. Controllers also need
  the **Input Monitoring** permission
  ([details](docs/BUILDING_MACOS.md#controllers)).
- **Linux:** a current distribution (the package is built on Ubuntu 26.04)
  and a Vulkan driver (Mesa or your GPU vendor's driver) are needed.

Settings and saves are kept beside the program, so the folder can live anywhere
you like. `SHA256SUMS` in the release lists the packages' checksums. Builds of
changes not yet released are under the
[**Build binaries** runs of `stable`](https://github.com/yannicksuter/mscharged-port/actions/workflows/build.yml?query=branch%3Astable+is%3Asuccess)
(*Artifacts*, signed-in users); see [GitHub builds](docs/BUILDING_GITHUB.md).

## Status

**Every offline feature of the original game works**: all modes, up to four
players, saves, movies, music and sound. What remains is polishing and testing.
Online play is not available, since Nintendo Wi-Fi Connection has shut down.

Highlights:

- Keyboard & mouse with freely assignable keys
- Xbox, PlayStation and other gamepads
- Real Wii Remotes with Nunchuk through a Mayflash DolphinBar, with pointer
  calibration
- Sharp picture, 4:3 or 16:9, optional antialiasing, any window size or
  fullscreen
- A launcher for disc, players, controls, display and audio

See the full **[feature list](docs/FEATURES.md)**.

### Builds

All three platforms are built and packaged by the
[**Build binaries**](https://github.com/yannicksuter/mscharged-port/actions/workflows/build.yml)
workflow (badge above: latest `stable` build).

| Platform | Build | Package | State |
| --- | --- | --- | --- |
| Linux x86_64 | Ubuntu 26.04, GCC 15, Vulkan | `.tar.gz` | Tested, playable |
| Windows x86_64 | Cross-compiled with LLVM-MinGW, Vulkan | `.zip` | Playable; tested on Windows |
| macOS Apple Silicon | macOS 15, Apple Clang, Metal | `.tar.gz` | Playable; tested on Apple Silicon |

## Game data

Supply an **ISO or RVZ from your own copy** of the USA disc. Game data is not
included. Choose the file in the launcher or see
[disc setup](docs/BUILDING.md#configure-your-disc).

## Controls

Keyboard & mouse act as a Wii Remote with a Nunchuk; gamepads and real Wii
Remotes can be assigned to players 1–4 in the launcher. Default keys:

| Wii input | Keyboard / mouse |
| --- | --- |
| Pointer | Mouse |
| A / B | Enter or left click / Esc or right click |
| D-pad | Arrow keys |
| Nunchuk stick | W A S D |
| Nunchuk C / Z | C / V |
| 1 / 2 | Z / X |
| + / − / HOME | Tab / - / Home |
| Shake Remote / Nunchuk | E / Q |

Press **P** for a screenshot. See [Wii Remote setup](docs/RUNTIME.md#wii-remote)
for the DolphinBar.

## Build from source

Install the [build prerequisites](docs/BUILDING.md#requirements) first; Mac users
can follow [macOS tool setup](docs/BUILDING_MACOS.md#install-tools). Then:

```sh
git clone --branch main --no-recurse-submodules https://github.com/yannicksuter/mscharged-port.git && cd mscharged-port
python3 tools/setup_dependencies.py && CMAKE_BUILD_PARALLEL_LEVEL=3 cmake --workflow --preset release --fresh
./build/release/mscharged
```

`main` is the development branch; `stable` holds reviewed snapshots. See the
[build guide](docs/BUILDING.md) for updates and options.

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
