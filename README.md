# Mario Strikers Charged — Native Port

A native C/C++ port of **Mario Strikers Charged** for Nintendo Wii, based on
[mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp), with
[Aurora](https://github.com/encounter/aurora) providing platform and rendering
support. This compiles reconstructed game source for modern systems rather than
recompiling the PowerPC game binary.

## Status

**Work in progress; the game is not playable yet.** The port advances alongside
the decompilation's reconstruction, cleanup, and validation. Development builds
are verified on Linux; Windows and macOS are intended targets.

## Build

Requires Git, CMake 3.25+, Ninja, Python 3.10+, a C/C++20 compiler, Rust/Cargo
1.85+, and SDL's platform development dependencies. Mac users should start with
the [macOS build guide](docs/BUILDING_MACOS.md). From the repository root:

```sh
git -c submodule.recurse=false submodule update --init --checkout -- extern/mscharged-decomp extern/nod extern/corrosion extern/sdl extern/imgui
cmake --workflow --preset release
./build/release/mscharged
```

This builds and opens the development launcher. For Debug, use
`cmake --workflow --preset debug` and `./build/debug/mscharged`.
See [build instructions](docs/BUILDING.md) for setup details and
[runtime checks](docs/RUNTIME.md) for experimental builds.

## Game data

Select an **ISO or RVZ from your own copy** in the launcher. Game data is not
included. Local files in `game/` and your personal `mscharged.ini` are excluded
from Git. See [disc setup](docs/BUILDING.md#configure-your-disc).

## Porting approach

The build copies the pinned decomp's complete `include/`, `libs/`, and `src/`
trees, applies reviewed compatibility patches, and compiles the prepared source
natively. The goal is to execute the original game flow from `main(...)`
throughout the game. Native adapters provide hardware services; patches address
compiler and platform differences while preserving retail behavior. Dependencies
remain clean submodules at explicit revisions, advanced through reviewed updates.

See the short [port strategy](docs/PORTING.md), [patch workflow](patches/README.md),
and [contributing guide](CONTRIBUTING.md).

## License

Original port code, tools, and documentation use [CC0 1.0 Universal](LICENSE).
Third-party material retains its own terms; see
[license scope and notices](LICENSES/README.md).

Mario Strikers Charged was developed by Next Level Games and published by
Nintendo. This is an unofficial project, unaffiliated with either company.
