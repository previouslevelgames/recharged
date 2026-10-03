# Runtime status

The port is a work in progress alongside the incomplete decompilation. The
launcher, disc access, selected original initialization, and a static model
preview work on Linux. **Menus and matches are not available yet.**

Start with the prerequisites and launcher dependencies in [Building](BUILDING.md).
The experimental presets add:

| Preset | Purpose |
| --- | --- |
| `aurora` | Host window, memory, and clock checks without game data. |
| `startup` | Selected original game initialization and disc reads. |
| `graphics` | Standalone Aurora GX/Vulkan rendering check without game data. |
| `scene` | Static Wii models, original materials, lighting, shadows and graphics frame lifecycle. |

## Experimental original startup

Initialize the additional dependencies, then build and run from the repository root:

```sh
git submodule update --init --checkout extern/aurora extern/abseil-cpp extern/fmt extern/xxhash extern/tracy
cmake --workflow --preset startup
./build/startup/mscharged --experimental-startup --config ./mscharged.ini
```

This requires a USA `R4QE01` revision 1 ISO/RVZ. It stops explicitly with exit code
3 after the implemented initialization checks; later startup stages remain pending.
The same build exposes **Try startup** in the launcher.
The checks include original memory, file access, boot configuration, tweak
registration, events, task scheduling, frame timing, camera diagnostics and camera
asset loading. Movies, authored camera playback and the complete game frame loop
remain in development.

For the host check alone, use `cmake --workflow --preset aurora`, then run
`./build/aurora/mscharged-aurora-check --window`.

## Independent GX/Vulkan diagnostic

Graphics builds currently require Linux, a desktop session, a Vulkan-capable
GPU and driver, Vulkan validation layers, GNU Make, and Tcl 8.6+.
Initialize these additional sources for both `graphics` and `scene`:

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

Build and run the standalone rendering check:

```sh
CMAKE_BUILD_PARALLEL_LEVEL=4 cmake --workflow --preset graphics
./build/graphics/mscharged-gx-check --window
```

The first graphics build is substantially larger than the launcher build. Adjust
`CMAKE_BUILD_PARALLEL_LEVEL` for your available memory. GPU tests are opt-in via
`-DMSCHARGED_TEST_VULKAN=ON` when configuring the preset.

## Experimental static Wii asset preview

With the graphics dependencies above and your disc configured:

```sh
CMAKE_BUILD_PARALLEL_LEVEL=4 cmake --workflow --preset scene
./build/scene/mscharged --experimental-scene --config ./mscharged.ini
```

The preview loads the ball from a USA `R4QE01` revision 1 image and renders it
through Aurora/Vulkan with selected original materials, lighting and texture
animation. Its diagnostic poses pass through the original camera core;
authored cameras remain pending.
An explicit `--world /DISC/gameworld.tmp.zlib --model-id HEX` selects a static world resource;
shadow-volume models use a diagnostic receiver. Full scenes and character
animation remain in development.

Press Escape or close the window to exit. Add `--frames 180` for a bounded run,
`--unlit` to compare lighting, or use `--help` for other preview options. Running
`./build/scene/mscharged` without arguments opens the launcher.
