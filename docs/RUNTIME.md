# Experimental runtime

The port is a work in progress alongside the decompilation. Linux diagnostics
cover selected original initialization, Wii assets and menu previews.
**Full original game startup and matches remain in development.** These experiments
currently require a USA `R4QE01` revision 1 ISO/RVZ configured in `mscharged.ini`.

Start with [Building](BUILDING.md) for compiler requirements, the launcher and
disc configuration. Host input and focus behavior are still being validated.

## Experimental original startup

```sh
git submodule update --init --checkout extern/aurora extern/abseil-cpp extern/fmt extern/xxhash extern/tracy extern/zlib-ng
cmake --workflow --preset startup
./build/startup/mscharged --experimental-startup --config ./mscharged.ini
```

This runs the available original initialization and stops explicitly with exit
code 3 at the unfinished startup boundary. The launcher exposes **Try startup**.
For a host check without game data, build preset `aurora` and run
`./build/aurora/mscharged-aurora-check --window`.

## Original Credits scene test

With the Linux/Vulkan prerequisites below:

```sh
cmake --preset graphics -DMSCHARGED_BUILD_ORIGINAL_CREDITS_DIAGNOSTIC=ON
cmake --build --preset graphics --target mscharged-original-main-credits-check -j 3
./build/graphics/mscharged-original-main-credits-check --disc ./game/R4QE01.rvz --window
```

This temporary test enters original `main`, then loads and renders Credits with
the original fonts, frontend code, scrolling and THP movie with native audio.
It skips blocked startup steps; full startup, game input and full game audio
remain pending. Playback timing is still experimental. Close the window to exit. Omit
`--window` to save a capture and exit. The test uses original frame submission
and VI framebuffer presentation. The game image keeps its original aspect and
positioning when the window is resized. Add `--size 1280x720` to choose an initial
window size; unused space is filled with bars.

The separate `mscharged-original-credits-check` target tests Credits directly,
including the original THP movie and native audio:

```sh
cmake --build --preset graphics --target mscharged-original-credits-check -j 3
./build/graphics/mscharged-original-credits-check --disc ./game/R4QE01.rvz --window
```

Audio can repeat during host scheduling delays. Full game audio remains pending.

## Original movie audio test

```sh
cmake --preset startup -DMSCHARGED_BUILD_THP_AUDIO_DIAGNOSTIC=ON
cmake --build build/startup --target mscharged-thp-audio-check
SDL_VIDEODRIVER=dummy ./build/startup/mscharged-thp-audio-check \
  ./build/startup/liboriginal_movie_audio_module.so ./game/R4QE01.rvz \
  art/movies/credits.thp 5000 ./build/startup/credits.pcm
```

This opt-in diagnostic runs the original movie decoder and mixer through the
native audio device. It omits video and full startup; audio can repeat during
host scheduling delays.

## Rendering and menu previews

Graphics presets currently require Linux, a desktop, Vulkan GPU/driver,
validation layers, GNU Make, and Tcl 8.6+. Initialize these additional sources:

```sh
git -c submodule.recurse=false submodule update --init --checkout \
  extern/aurora extern/dawn extern/fmt extern/xxhash extern/tracy \
  extern/zlib-ng extern/libpng extern/freetype extern/sqlite extern/zstd
git -C extern/dawn -c submodule.recurse=false submodule update --init --checkout --depth 1 -- \
  third_party/abseil-cpp third_party/jinja2 third_party/markupsafe \
  third_party/spirv-headers/src third_party/spirv-tools/src \
  third_party/vulkan-headers/src third_party/vulkan-utility-libraries/src
git -C extern/freetype -c submodule.recurse=false submodule update --init --checkout --depth 1 -- subprojects/dlg
CMAKE_BUILD_PARALLEL_LEVEL=4 cmake --workflow --preset scene
./build/scene/mscharged --experimental-scene --frontend-title --config ./mscharged.ini
```

Arrows, D-pad or the left stick move the pointer; Enter/A selects and Escape/B
goes Back. Options includes Audio, Visual and the earlier Credits preview.
These previews use transitional adapters and do not validate execution of the
full original game or the refactored original Credits scene.

Use `--frontend-main` or `--frontend-options` to start there directly, or omit
the menu selection for the static ball preview. Additional inspectors include `--particles`,
`--debug-camera`, and `--frontend-boot`; use `--help` for their options.
Add `--frames 180` for a bounded run. Run without arguments to open the launcher.

For a rendering check without game data, build preset `graphics` and run
`./build/graphics/mscharged-gx-check --window`. The first graphics build is large;
adjust `CMAKE_BUILD_PARALLEL_LEVEL` for available memory. Vulkan tests are opt-in
with `-DMSCHARGED_TEST_VULKAN=ON` when configuring.

Full Wii peripheral support, other disc regions, Windows and macOS remain
unverified. These selected runtime paths do not complete the original game loop.
