# Experimental runtime

The port is a work in progress alongside the decompilation. Selected tests run
original game code on Linux/Vulkan; **full startup and matches are unfinished**.
These tests currently require a USA `R4QE01` revision 1 ISO/RVZ. See
[Building](BUILDING.md) for the default launcher build and disc configuration.

## Graphics prerequisites

Use a Vulkan GPU/driver, a desktop, validation layers, GNU Make and Tcl 8.6+.
Initialize the additional pinned sources:

```sh
git -c submodule.recurse=false submodule update --init --checkout \
  extern/aurora extern/dawn extern/fmt extern/xxhash extern/tracy \
  extern/zlib-ng extern/libpng extern/freetype extern/sqlite extern/zstd
git -C extern/dawn -c submodule.recurse=false submodule update --init --checkout --depth 1 -- \
  third_party/abseil-cpp third_party/jinja2 third_party/markupsafe \
  third_party/spirv-headers/src third_party/spirv-tools/src \
  third_party/vulkan-headers/src third_party/vulkan-utility-libraries/src
git -C extern/freetype -c submodule.recurse=false submodule update --init --checkout --depth 1 -- subprojects/dlg
```

The first graphics build is large. Adjust the build parallelism for available
memory. Windows, macOS, other disc regions and Wii peripherals remain unverified.

## Original Boot scene

```sh
CMAKE_BUILD_PARALLEL_LEVEL=3 cmake --workflow --preset frontend
./build/graphics/mscharged --experimental-frontend --disc ./game/R4QE01.rvz --window
```

The `frontend` preset reuses `build/graphics`. It runs original `main`, loading
and frontend tasks through the authored Boot script, including the static Next
Level logo and its original sound request. Earlier startup steps are still
omitted; Intro movie initialization and progression to later menus remain in
development. Audio quality and performance are still being validated.

The launcher offers **Try boot sequence**. Close the window to exit, or omit
`--window` for a bounded run. A build with the authored Boot script enabled does
not support the separate `--experimental-options` shortcut.

## Original Credits scene

After building the `frontend` preset:

```sh
./build/graphics/mscharged --experimental-credits --disc ./game/R4QE01.rvz --window
```

This selected test runs original Credits loading, text, scrolling and THP video
with native audio. It skips unfinished startup steps. Movie audio can still
repeat during host scheduling delays. Enter/Space is A, Escape/Backspace is B,
arrows are D-pad, and Z/X are 1/2. A advances to COPYRIGHTS; the following menu
transition is unfinished. Host focus and menu input remain under validation.

## Run options

Both tests accept `--config FILE`, `--disc FILE` (also `--disk`), `--window`,
`--fullscreen`, `--size WIDTHxHEIGHT` and `--aspect auto|4:3|16:9`. These override
INI settings for this run without saving. The default layout is 16:9; resizing
preserves the selected layout and fills unused space with bars.

## Other checks

For the original initialization boundary without graphics:

```sh
git submodule update --init --checkout extern/aurora extern/abseil-cpp extern/fmt extern/xxhash extern/tracy extern/zlib-ng
cmake --workflow --preset startup
./build/startup/mscharged --experimental-startup --config ./mscharged.ini
```

This stops explicitly at the unfinished startup boundary with exit code 3.
For rendering without game data, build preset `graphics` and run
`./build/graphics/mscharged-gx-check --window`.
