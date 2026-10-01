# Building the development bootstrap

The current target compiles three available decompilation translation units:
`src/NL/nlEndian.cpp`, `src/NL/nlRandom.cpp`, and
`src/Game/Core/mtRandom.cpp`. It runs small native utility checks. It does not
load game data, render graphics, or run a match.

The decompilation is incomplete and not fully linked. Missing functions,
unfinished reconstructions, console assembly, Wii services, and incompatible
layouts will be handled incrementally. A passing bootstrap build is evidence
for this source subset only. The first 100% decompilation and 100% link release
is a future upstream milestone, not a prerequisite for working on this setup.

## Requirements

- Git, including submodule support.
- Python 3.10 or newer; no third-party Python packages are needed.
- CMake 3.25 or newer.
- Ninja, or another CMake-supported build tool.
- A C++17 compiler. The bootstrap has been validated with Clang and GCC on Linux.

Windows and macOS are port targets. Their build commands are provided for
development but have not yet been executed on those systems. This small target
does not require Rust, Qt, a graphics SDK, or game data.

## Fetch the source

From a checkout of this repository, initialize the dependency used by the
bootstrap at its recorded commit:

```sh
git submodule update --init --checkout extern/mscharged-decomp
```

To also download all top-level library sources for runtime and UI development:

```sh
git -c submodule.recurse=false submodule update --init --checkout
```

The [dependency inventory](../extern/README.md) explains nested dependencies and
remaining integration work. Dawn has a large set of nested development and
platform dependencies; full recursive initialization is unnecessary for this
bootstrap. Ordinary builds do not fetch or advance upstream revisions.

## Linux and macOS

Run these commands from the repository root:

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/mscharged-bootstrap
```

The executable identifies itself as a development bootstrap and prints the
decomp commit used. Test failures return a nonzero exit status.

## Windows

Use a Visual Studio developer PowerShell with the C++ tools, Python, CMake, and
Ninja available. CMake can select the compiler provided by that environment:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
.\build\mscharged-bootstrap.exe
```

Use a separate build directory when changing compilers. Supported local output
locations are `build/`, `build-*/`, `cmake-build-*/`, or `out/` inside the checkout.
These directories are ignored by Git. In-source builds are rejected.

## What happens before compilation

1. Read the decomp gitlink from the port's Git index and require that exact
   checkout. On a fresh clone this is the committed pin; during an intentional
   dependency update it is the newly staged pin.
2. Reject tracked edits inside the dependency. Ignored and untracked local
   files are not exported.
3. Export committed sources, including any required initialized nested
   submodules at their pins, to a temporary build directory.
4. Validate the patch base and apply `patches/mscharged-decomp/series` in order.
   A failure prevents publication of the prepared tree.
5. Publish `build/prepared/mscharged-decomp/source/` and a manifest recording
   commits, ordered patch hashes, preparation-tool hash, and generated contents.
6. Configure the selected source list. Before each build, verify that the
   recorded inputs and generated contents still match.

CMake watches the patch directory, preparation script, and Git index. If a
manual dependency checkout or generated edit causes validation to fail, resolve
the reported mismatch and configure again. An old executable may remain on disk
after a failure; it is not a successful build of the new inputs.

Manual preparation and verification are also available:

```sh
python3 tools/prepare_sources.py --build-dir build
python3 tools/prepare_sources.py --build-dir build --check
```

On Windows, use `python` if that is the installed command name.

## Tests and development scope

`foundation` checks fixed-width types, vector layouts, endian conversion,
deterministic random values, range handling, and the portable absolute-value
helper. The MT source currently exposes seed initialization only; there is no
claim of a complete MT implementation or game simulation validation.

`source_preparation` uses disposable local Git fixtures. It checks ordered
patches, cache reuse, pin/base validation, tracked changes, excluded local data,
failed preparation, stale/edited output, patch export, and nested submodules.

To run only the preparation suite:

```sh
python3 -B tests/test_prepare_sources.py
```

Aurora, UI, complete game linking, and packaging remain future integration
steps. Add source units and checks deliberately as the decomp becomes usable.
See [patch development](../patches/README.md) before editing generated sources.
