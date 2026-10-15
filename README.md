# Mario Strikers Charged — Native Port

[![Build stable](https://github.com/yannicksuter/mscharged-port/actions/workflows/build.yml/badge.svg?branch=stable)](https://github.com/yannicksuter/mscharged-port/actions/workflows/build.yml?query=branch%3Astable)
[![Version 1.0.10](https://img.shields.io/badge/version-1.0.10-blue)](CMakeLists.txt)
[![License CC0 1.0](https://img.shields.io/badge/license-CC0%201.0-lightgrey)](LICENSE)

Play **Mario Strikers Charged** natively on your PC. The port compiles the
fully reconstructed game source of
[mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp) into native
code, with [Aurora](https://github.com/encounter/aurora) in place of the Wii
hardware. It is not an emulator and not a recompilation of the PowerPC binary:
the original game runs unchanged, modern systems provide the rest.

> [!IMPORTANT]
> **The downloads contain no game files.** To play you need your own copy of
> **Mario Strikers Charged, USA version (game ID `R4QE01`, revision 1)** as an
> ISO or RVZ disc image. European and Japanese discs are not supported.

## Download

Pick your system on the [**latest release**](https://github.com/yannicksuter/mscharged-port/releases/latest)
page. Unpack the package, start `mscharged` (`mscharged.exe` on Windows), choose
your disc image in the launcher and press **Play**.

<table>
<tr>
<td align="center" width="20%">
<a href="https://github.com/yannicksuter/mscharged-port/releases/latest"><img alt="Windows" src="https://img.shields.io/badge/Windows-10%20%2F%2011%20%C2%B7%20x64-0078D4?logo=windows&logoColor=white"></a><br>
<code>windows-x86_64.zip</code><br>
Playable, tested
</td>
<td align="center" width="20%">
<a href="https://github.com/yannicksuter/mscharged-port/releases/latest"><img alt="Linux" src="https://img.shields.io/badge/Linux-x86__64%20%C2%B7%20Vulkan-FCC624?logo=linux&logoColor=black"></a><br>
<code>linux-x86_64.tar.gz</code><br>
Playable, tested
</td>
<td align="center" width="20%">
<a href="docs/STEAM_DECK.md"><img alt="Steam Deck" src="https://img.shields.io/badge/Steam%20Deck-SteamOS%203.7%2B-1A9FFF?logo=steam&logoColor=white"></a><br>
<code>steamdeck-x86_64.tar.gz</code><br>
<a href="docs/STEAM_DECK.md">Guide</a> · not yet tested on a Deck
</td>
<td align="center" width="20%">
<a href="https://github.com/yannicksuter/mscharged-port/releases/latest"><img alt="macOS Apple Silicon" src="https://img.shields.io/badge/macOS-Apple%20Silicon-0A84FF?logo=apple&logoColor=white"></a><br>
<code>macos-arm64.tar.gz</code><br>
Playable, tested
</td>
<td align="center" width="20%">
<a href="https://github.com/yannicksuter/mscharged-port/releases/latest"><img alt="macOS Intel" src="https://img.shields.io/badge/macOS-Intel%20%C2%B7%2015%2B-0A84FF?logo=apple&logoColor=white"></a><br>
<code>macos-x86_64.tar.gz</code><br>
Not yet tested
</td>
</tr>
</table>

All packages are built by the
[Build binaries](https://github.com/yannicksuter/mscharged-port/actions/workflows/build.yml)
workflow; `SHA256SUMS` on the release lists their checksums. Step-by-step
instructions, including the first-start prompts of Windows and macOS, are in
the **[install guide](docs/INSTALL.md)**; Steam Deck owners should follow the
**[Steam Deck guide](docs/STEAM_DECK.md)**.

## Status

**Every offline feature of the original game works**: all modes, up to four
players, saves, movies, music and sound. What remains is polishing and testing.
Online play is not available, since Nintendo Wi-Fi Connection has shut down.

- Keyboard & mouse, Xbox, PlayStation and other gamepads, and real Wii Remotes
  with Nunchuk (Bluetooth or Mayflash DolphinBar)
- Sharp picture, 4:3 or 16:9, optional antialiasing, any window size or fullscreen
- High frame rates: 120, 144, your display's rate or unlimited
- A launcher for disc, players, controls, display and audio, with in-place updates

See the full **[feature list](docs/FEATURES.md)** and the
[default controls](docs/FEATURES.md#default-bindings). If something goes wrong,
attach `logs/mscharged.log` (beside the program) to an
[issue](https://github.com/yannicksuter/mscharged-port/issues).

## Build it yourself

Install the [build prerequisites](docs/BUILDING.md#requirements) (Mac users:
[macOS tool setup](docs/BUILDING_MACOS.md#install-tools)), then:

```sh
git clone --branch main --no-recurse-submodules https://github.com/yannicksuter/mscharged-port.git && cd mscharged-port
python3 tools/setup_dependencies.py && CMAKE_BUILD_PARALLEL_LEVEL=3 cmake --workflow --preset release --fresh
./build/release/mscharged
```

`main` is the development branch; `stable` holds reviewed snapshots. You still
need your own disc image ([disc setup](docs/BUILDING.md#configure-your-disc)).

## Documentation

| I want to… | Read |
| --- | --- |
| Install and play | [Install guide](docs/INSTALL.md) |
| Play on a Steam Deck | [Steam Deck guide](docs/STEAM_DECK.md) |
| See what is supported | [Features](docs/FEATURES.md) |
| Set up Wii Remotes, saves, updates, run options | [Runtime guide](docs/RUNTIME.md) |
| Build from source | [Build guide](docs/BUILDING.md), [macOS](docs/BUILDING_MACOS.md) |
| Get test builds and CI packages | [GitHub builds](docs/BUILDING_GITHUB.md) |
| Understand how the port works | [Port strategy](docs/PORTING.md), [patch workflow](patches/README.md) |
| Contribute | [Contributing](CONTRIBUTING.md) |
| Check licences | [License scope and notices](LICENSES/README.md) |

## How it works

Original source plus reviewed patches becomes the compiled game: the decomp
and dependencies are pinned, the decomp's `include/`, `libs/` and `src/` are
exported and patched for modern compilers and native platform services,
formatted for readability and compiled. The original code controls the game
from `main(...)`. Details, including how to prepare the source without
building, are in the [port strategy](docs/PORTING.md).

## License

Original port code, tools, and documentation use [CC0 1.0 Universal](LICENSE).
Third-party material retains its own terms; see
[license scope and notices](LICENSES/README.md).

Mario Strikers Charged was developed by Next Level Games and published by
Nintendo. This is an unofficial project, unaffiliated with either company.
