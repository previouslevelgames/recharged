# Building the development bootstrap

The current target compiles three available decompilation translation units:
`src/NL/nlEndian.cpp`, `src/NL/nlRandom.cpp`, and
`src/Game/Core/mtRandom.cpp`. It runs small native utility checks and uses nod
to identify ISO/RVZ images and open their game data partition. It does not
render graphics or run a match.

The decompilation is incomplete and not fully linked. Missing functions,
unfinished reconstructions, console assembly, Wii services, and incompatible
layouts will be handled incrementally. A passing bootstrap build is evidence
for this source subset only. The first 100% decompilation and 100% link release
is a future upstream milestone, not a prerequisite for working on this setup.

## Requirements

- Git, including submodule support.
- Python 3.10 or newer; no third-party Python packages are needed.
- CMake 3.25 or newer.
- Ninja for the supplied presets, or another CMake-supported tool for a manual build.
- C and C++17 compilers. The initial utilities were validated with Clang and GCC
  on Linux; the disc-enabled bootstrap was validated with GCC 16.
- Rust and Cargo 1.85 or newer for nod; validated with Rust 1.95 on Linux.

Windows and macOS are port targets. Their build commands are provided for
development but have not yet been executed on those systems. No Qt or graphics
SDK is required. Building, tests, and `--self-test` need no game data.

The first nod build downloads the Rust crates recorded in its committed
`Cargo.lock`. Cargo runs with `--locked` and cannot silently update those
versions. Compression libraries are currently built from Cargo's pinned source
crates with support for Bzip2, LZMA/LZMA2, zlib, and Zstandard. Subsequent builds
can use the Cargo cache offline (`CARGO_NET_OFFLINE=true`).

## Fetch the source

From a checkout of this repository, initialize the dependencies used by the
bootstrap at their recorded commits:

```sh
git submodule update --init --checkout extern/mscharged-decomp extern/nod extern/corrosion
```

To also download all top-level library sources for runtime and UI development:

```sh
git -c submodule.recurse=false submodule update --init --checkout
```

The [dependency inventory](../extern/README.md) explains nested dependencies and
remaining integration work. Dawn has a large set of nested development and
platform dependencies; full recursive initialization is unnecessary for this
bootstrap. Ordinary builds do not fetch or advance upstream revisions.

## Build and run

From the repository root, one command configures the Release build, prepares
patched sources, compiles, and runs all three test suites:

```sh
cmake --workflow --preset release
```

The `release` preset selects compiler optimizations. This is still a development
bootstrap with the scope described above.

On Linux and macOS:

```sh
./build/release/mscharged-bootstrap --version
./build/release/mscharged-bootstrap --self-test
```

The first command prints the build version. `--self-test` also prints the
decomp commit and checks native utility behavior. Running without arguments
loads your configured disc as described below. Failures return a nonzero exit
status; `--help` lists the supported arguments.

## Configure your disc

1. Put your ISO or RVZ in `game/`, for example `game/R4QE01.rvz`. All contents
   except the `.gitkeep` placeholder are ignored by Git.
2. Copy `mscharged.ini.example` to `mscharged.ini` (`cp` on Linux/macOS or
   `Copy-Item` in PowerShell). The personalized file is ignored by Git.
3. Set `[game] disc` to your image's path. Relative paths start at the INI's
   directory; absolute paths also work. Paths containing spaces may be quoted.
   Save the INI as UTF-8; a UTF-8 BOM and CRLF line endings are accepted.
4. Run `./build/release/mscharged-bootstrap` from the repository root.

Example configuration:

```ini
[game]
disc = game/R4QE01.rvz
```

You can keep the image elsewhere, or choose an explicit configuration:

```sh
./build/release/mscharged-bootstrap --disc "/path/to/Mario Strikers Charged.iso"
./build/release/mscharged-bootstrap --config "/path/to/local.ini"
```

ISO and RVZ are opened directly by nod using their contents to detect the format.
Neither the source image nor the decomp checkout is modified. The check reports
the disc ID, revision, format, title, and game file count. A missing/unreadable
image, another game's disc, or an unusable data partition produces an error.
This is a metadata/readability check, not a full-disc checksum verification.

The source baseline is USA `R4QE01`, disc revision 1. Other Charged discs can
be inspected, with a message that their asset compatibility is unverified.
A successful check still does not run the game. ISO reading is tested with a
generated Wii fixture; RVZ reading was also checked with a local USA revision 1
image, reading a file table containing 2,354 game files.

### Windows

Use a Visual Studio developer PowerShell with the C++ tools, Python, CMake, and
Ninja available. CMake can select the compiler provided by that environment:

```powershell
cmake --workflow --preset release
.\build\release\mscharged-bootstrap.exe --version
.\build\release\mscharged-bootstrap.exe --self-test
```

### Debug and incremental builds

For a build with debug information:

```sh
cmake --workflow --preset debug
./build/debug/mscharged-bootstrap
```

On Windows, run `.\build\debug\mscharged-bootstrap.exe` instead. Debug and Release
outputs use separate directories, so both can coexist. After editing sources,
you can rebuild and test separately:

```sh
cmake --build --preset debug
ctest --preset debug
```

Use `release` in both commands for the Release configuration. To choose a
compiler, set it on the first configuration of that directory, for example:

```sh
cmake --preset debug -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset debug
ctest --preset debug
```

Manual configuration remains supported, including existing `build/` directories:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Use a separate build directory when changing compilers. Supported local output
locations are `build/`, `build-*/`, `cmake-build-*/`, or `out/` inside the checkout.
These directories are ignored by Git. In-source builds are rejected.

## Build versions

The current version is `0.0.1-dev`, defined in the root `CMakeLists.txt`.
Every executable includes the port commit in this form:

```text
0.0.1-dev+g<commit>
0.0.1-dev+g<commit>.dirty
```

`<commit>` is the abbreviated port Git revision, normally 12 hexadecimal
characters. `.dirty` indicates uncommitted tracked changes, untracked files
that Git does not ignore, or submodule changes. Ignored local files and build
outputs do not affect the version. The revision and change status refresh on
each build, including incremental builds after a commit.

The executable also reports its Debug or Release configuration. The port's
version is independent of the decompilation's planned `1.0` release; a normal
bootstrap run prints the exact decomp revision separately. These development
builds do not require a release tag.

## What happens before compilation

1. Read the decomp, nod, and Corrosion gitlinks from the port's Git index and
   require those exact checkouts. On a fresh clone these are the committed pins;
   during an intentional dependency update they are the newly staged pins.
2. Reject tracked edits inside the dependency. Ignored and untracked local
   files are not exported.
3. Export committed sources, including any required initialized nested
   submodules at their pins, to a temporary build directory.
4. Validate each patch base and apply its series in order. The decomp series
   adapts native utilities; the nod series enforces the Cargo lockfile.
   A failure prevents publication of that dependency's prepared tree.
5. Publish `<build-dir>/prepared/<dependency>/source/` and a manifest
   recording commits, ordered patch hashes, preparation-tool hash, and generated
   contents. For the Release preset, `<build-dir>` is `build/release`.
6. Configure the selected source list. Before each build, verify that the
   recorded inputs and generated contents still match.

CMake watches the patch directory, preparation script, and Git index. If a
manual dependency checkout or generated edit causes validation to fail, resolve
the reported mismatch and configure again. An old executable may remain on disk
after a failure; it is not a successful build of the new inputs.

Manual preparation and verification are also available:

```sh
python3 tools/prepare_sources.py --build-dir build/release
python3 tools/prepare_sources.py --build-dir build/release --check
```

Use the same build directory when preparing sources, exporting patches, and
compiling. On Windows, use `python` if that is the installed command name.
Add `--dependency nod` or `--dependency corrosion` to work with those sources.
nod's Corrosion FetchContent request uses our prepared local submodule.

The selected launcher image is copied to `<build-dir>/assets/launcher/header.png`.
Its [source attribution](../assets/launcher/README.md) is separate from the port's
CC0 code. The command-line executable does not display artwork yet.

## Tests and development scope

`foundation` checks fixed-width types, vector layouts, endian conversion,
deterministic random values, range handling, and the portable absolute-value
helper. The MT source currently exposes seed initialization only; there is no
claim of a complete MT implementation or game simulation validation.

`source_preparation` uses disposable local Git fixtures. It checks ordered
patches, cache reuse, pin/base validation, tracked changes, excluded local data,
failed preparation, stale/edited output, patch export, and nested submodules.

`bootstrap_cli` checks INI setup, relative paths, spaces, BOM/CRLF handling,
ISO data-partition access, wrong-game rejection, missing partitions, malformed
images, and command-line errors. It generates a small synthetic Wii image
containing original test data; no proprietary disc contents are needed.

To run only the preparation suite:

```sh
python3 -B tests/test_prepare_sources.py
```

Aurora, UI, complete game linking, and packaging remain future integration
steps. Add source units and checks deliberately as the decomp becomes usable.
See [patch development](../patches/README.md) before editing generated sources.
