# Mario Strikers Charged — Native Port

A native C/C++ port of **Mario Strikers Charged** for modern systems, based on
[mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp) and
[Aurora](https://github.com/encounter/aurora). It compiles reconstructed game
source into native machine code; it is not a recompilation of the PowerPC binary.

## Status

**Work in progress.** The port grows alongside the unfinished decompilation.
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

Use `main`, not the older `master` branch. The helper initializes only the pinned
dependencies required by your platform. See the [build guide](docs/BUILDING.md)
for updates and the smaller launcher build, and [runtime instructions](docs/RUNTIME.md)
for separate diagnostics.

## Game data

Supply an **ISO or RVZ from your own copy**. Game data is not included. Files in
`game/` and your personal `mscharged.ini` are excluded from Git.
See [disc setup](docs/BUILDING.md#configure-your-disc).

## Porting approach

The build exports the pinned decomp's complete `include/`, `libs/`, and `src/`
trees, then applies reviewed compiler and platform compatibility patches.
Original game code controls the game flow from `main(...)`; native adapters
replace Wii hardware services while preserving retail behavior. Dependencies
stay in clean, pinned submodules and advance through reviewed updates.

See the short [port strategy](docs/PORTING.md), [patch workflow](patches/README.md),
and [contributing guide](CONTRIBUTING.md).

## License

Original port code, tools, and documentation use [CC0 1.0 Universal](LICENSE).
Third-party material retains its own terms; see
[license scope and notices](LICENSES/README.md).

Mario Strikers Charged was developed by Next Level Games and published by
Nintendo. This is an unofficial project, unaffiliated with either company.
