# Building on macOS

The default build provides the launcher and ISO/RVZ disc checks. **macOS builds
are not yet verified.** The experimental game/menu runtime currently requires
Linux; the `frontend`, `graphics`, and `scene` presets cannot run on macOS yet.

## Install tools

Install Apple's Command Line Tools and finish the installer before continuing:

```sh
xcode-select --install
```

Install [Homebrew](https://docs.brew.sh/Installation), follow its instructions to
add it to your shell, then install the build tools:

```sh
brew install cmake ninja python rust
```

If you already use Rust through rustup, keep that installation; Rust/Cargo 1.85+
is required. SDL is built from the pinned source, so no Homebrew SDL package is
needed. On Apple Silicon, use a native terminal and native tools throughout.

## Clone and initialize dependencies

Use a Git clone, not GitHub's ZIP download: source preparation reads the recorded
submodule revisions from Git. Authenticate with GitHub if the repository is private.

```sh
git clone --no-recurse-submodules https://github.com/yannicksuter/mscharged-port.git
cd mscharged-port
git -c submodule.recurse=false submodule update --init --checkout -- \
  extern/mscharged-decomp extern/nod extern/corrosion extern/sdl extern/imgui
```

For an existing clone, enter its root and run the submodule command above.
The `extern/` folders are expected: they are **tracked submodules**, each pinned
to an exact revision. Do not add them to `.gitignore` or use `--remote` to update
them. Other dependency folders may remain empty; the default build only needs
the ones listed above. Generated sources and binaries go into ignored `build/`.

## Build and run

Run these commands from the repository root. `--fresh` clears an earlier CMake
configuration without deleting source or personal settings.

```sh
cmake --fresh --preset release \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++
cmake --build --preset release --parallel 3
ctest --preset release
./build/release/mscharged
```

Select your own disc image in the launcher, or follow [disc setup](BUILDING.md#configure-your-disc).
This build opens the launcher; it does not enter the game menus. For later code
changes, repeat the build command. After pulling repository updates, repeat the
submodule command and configure step too.

## If configuration fails

For `Tracked changes in .../extern/nod`, inspect the changes with
`git -C extern/nod diff HEAD`. To preserve tracked edits in that dependency's
local Git stash, restore its pinned checkout, and retry:

```sh
git -C extern/nod stash push -m "Local nod changes before port build"
git -c submodule.recurse=false submodule update --init --checkout -- extern/nod
cmake --workflow --preset release
```

The stash remains available through `git -C extern/nod stash list`. Keep it
unapplied while building: dependencies must match their recorded source.

`execute_process` / “failed command indexes” is CMake's failure summary. The
Git or `Source preparation failed` message immediately above it identifies the
cause. Missing or unexpected submodule revisions require the update command
above. If it reports local source edits, preserve those edits before proceeding.

To capture the complete configure error, run:

```sh
mkdir -p build
set -o pipefail
cmake --preset release 2>&1 | tee build/macos-configure.log
```

Include that log, your macOS version, Apple Silicon/Intel, and the output of
`clang++ --version` when reporting a failure. A Linux build result does not
establish macOS compatibility.
