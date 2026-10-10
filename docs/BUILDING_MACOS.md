# Building on macOS

Release includes the incomplete original-source frontend runtime and selects
Metal on macOS. **macOS runtime validation is still in progress.** Linux results
do not establish macOS compatibility or a complete playable game.

## Install tools

Install Apple's Command Line Tools and finish the installer:

```sh
xcode-select --install
```

Install [Homebrew](https://docs.brew.sh/Installation), follow its shell setup,
then install the remaining tools:

```sh
brew install cmake ninja python rust tcl-tk clang-format
export PATH="$(brew --prefix tcl-tk)/bin:$PATH"
```

Tcl 8.6+ is required; older macOS system Tcl is insufficient. If you use rustup,
keep that installation instead of installing Rust twice; Cargo 1.85+ is required.
Use native tools and a native terminal on Apple Silicon. SDL and graphics libraries are built from pinned source.

## Clone and initialize dependencies

Use a Git clone, not GitHub's ZIP download. Authenticate if the repository is
private. With your own USA `R4QE01` revision 1 ISO/RVZ ready, run:

```sh
git clone --branch main --no-recurse-submodules https://github.com/yannicksuter/mscharged-port.git && cd mscharged-port
python3 tools/setup_dependencies.py && CMAKE_BUILD_PARALLEL_LEVEL=3 cmake --workflow --preset release --fresh
./build/release/mscharged --disc /path/to/R4QE01.rvz --window
```

Quote paths containing spaces. These commands build the `main` development branch.
The helper initializes only required recorded revisions, including the selected
Metal dependencies. It does not update remote branch tips or discard local edits.

For an existing checkout, follow the explicit `origin main`
[update commands](BUILDING.md#build-and-run), then repeat setup and the Release
workflow. See [disc settings](BUILDING.md#configure-your-disc).
`--fresh` clears the CMake configuration without deleting sources or settings.
For later code changes, use `cmake --build --preset release`.
If presets or build options changed, run `cmake --preset release` first.

## If configuration fails

Read the `Source preparation failed` or Git message above CMake's failure
summary. Inspect reported dependency edits with `git -C extern/NAME diff HEAD`
and preserve them before retrying setup. Do not force-reset dependencies.

If CMake selected an older system Tcl, configure with the installed executable:

```sh
cmake --fresh --preset release -DMSCHARGED_SQLITE_TCLSH="$(brew --prefix tcl-tk)/bin/tclsh"
cmake --build --preset release --parallel 3
```

When reporting a failure, include the configure/build output, macOS version,
Apple Silicon or Intel, and `clang++ --version`.

## Controllers

macOS only lets an app read game controllers and Wii Remotes after you allow
**Input Monitoring** for the app that starts the game (Terminal, iTerm, …) in
System Settings → Privacy & Security. Restart that app afterwards, then
unplug and reconnect the controller: one that was already connected stays
silent until it is opened again.
