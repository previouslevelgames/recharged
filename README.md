# Mario Strikers Charged — Native Port

A native source port of **Mario Strikers Charged** for Nintendo Wii, built from
[mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp) with
[Aurora](https://github.com/encounter/aurora) as its compatibility and rendering
foundation.

**This is not a binary recompilation project.** The goal is a full native port
built by adapting the reconstructed C/C++ game and engine source to modern
systems.

## Status

**Work in progress.** The port grows alongside the ongoing decompilation,
cleanup, and validation of the original game. Development builds are currently
verified on Linux; Windows and macOS are intended targets.
**The game is not playable yet.**

## Why a native source port?

Compared with translating console machine instructions, working directly with
reconstructed source makes it easier to:

- **Debug and maintain the game:** follow readable gameplay and engine code,
  types, and data structures when diagnosing problems.
- **Integrate modern platforms:** adapt engine services and memory layouts for
  native graphics, audio, and input APIs.
- **Improve and extend the game:** refactor and optimize code or add features
  through explicit, reviewable source changes while preserving original behavior.

## Build

From the repository root:

```sh
git submodule update --init --checkout extern/mscharged-decomp extern/nod extern/corrosion extern/sdl extern/imgui
cmake --workflow --preset release
./build/release/mscharged
```

This builds and opens the development launcher. See
[build instructions](docs/BUILDING.md) for prerequisites, Debug builds, and
experimental runtime checks.

## Game data

Use an **ISO or RVZ** from your own copy of Mario Strikers Charged and select it
in the launcher. Game data is not included with this project.
See [disc setup](docs/BUILDING.md#configure-your-disc) for configuration details.

## Source and development

The decompilation, Aurora, and other source dependencies are Git submodules
pinned to specific commits. Before building, the port applies its patches to
generated source copies, keeping upstream checkouts clean. Dependency updates
are reviewed explicitly, so new upstream commits do not change an existing
port checkout.

- [Dependencies and attribution](extern/README.md)
- [Patch workflow](patches/README.md)
- [Current runtime implementation](docs/RUNTIME.md)
- [Contributing](CONTRIBUTING.md)

## License

Original port code, tools, and documentation use [CC0 1.0 Universal](LICENSE).
Third-party material retains its own terms; see
[license scope and notices](LICENSES/README.md).

Mario Strikers Charged was developed by Next Level Games and published by
Nintendo. This is an unofficial project, unaffiliated with either company.
