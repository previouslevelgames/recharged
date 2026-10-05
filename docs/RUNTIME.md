# Runtime status

The port is a work in progress alongside the incomplete decompilation. The
launcher, disc access, selected original initialization, and a static world
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
git submodule update --init --checkout extern/aurora extern/abseil-cpp extern/fmt extern/xxhash extern/tracy extern/zlib-ng
cmake --workflow --preset startup
./build/startup/mscharged --experimental-startup --config ./mscharged.ini
```

This requires a USA `R4QE01` revision 1 ISO/RVZ. It stops explicitly with exit code
3 after the implemented initialization checks; later startup stages remain pending.
The same build exposes **Try startup** in the launcher.
The checks include original memory, file access, boot configuration, tweak
registration, events, task scheduling, frame timing and authored camera loading
and playback. A separate bounded diagnostic executes the original boot script
until it reaches an unsupported service, currently particle loading. Movies and
the complete game frame loop remain in development. Particle files and textures
can be read and retained, with supported effect groups resolved through original
code. Supported emitter groups run original particle simulation with shared
storage and ordered controller updates. Geometry registration and the complete
effects manager are still pending. The scene preview below renders two group
instances.

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

Add `--particles` to render two instances of a complete authored emitter group,
with pause, visibility and reset controls. They share the original particle pool,
atlas, RNG and textures, using original simulation and drawing rules. This preview
does not start the complete effects manager and cannot be combined with PIP or
shadow previews.

The preview loads the ball from a USA `R4QE01` revision 1 image and renders it
through Aurora/Vulkan with selected original materials, lighting and texture
animation. Its poses pass through the original camera core. Use
`--camera /DISC/camera.cam` to play an authored track with original timing and
transforms; choose geometry in that camera's world coordinates. Bounded runs use
a fixed 60 Hz camera clock. Depth-of-field rendering remains pending.
Use `--debug-camera` instead for interactive inspection: arrows orbit, WASD pans,
Q/E changes radius, Shift+Q/E changes height, and R resets the pose. Gamepads are
also supported; bindings appear in the preview. Gameplay input remains in
development. Authored and debug cameras cannot be combined.
An explicit `--world /DISC/gameworld.tmp.zlib --model-id HEX` selects a static world resource;
shadow-volume models use a diagnostic receiver. To render object instances, also
provide `--world-res /DISC/gameworld.res.zlib` and repeat `--object-id HEX` for
each selected object, instead of `--model-id`. This loads shared models/textures
and preserves each object's transform. Up to 256 supported static objects can
be selected; missing or unsupported objects report an error. Original sphere/box
culling follows the active camera, with opaque and transparent packets in separate
passes. Use `--no-world-culling` or the preview toggle for comparison. Full scene
loading, visibility hierarchies and character animation remain in development.
Use `--experimental-scene --character-shock` to preview Bowser's shock mesh
with original bone animation and skin materials. Multiple-weight skinning,
morphs and the complete character controller remain in development.

To preview the supported static objects in the frontend environment:

```sh
./build/scene/mscharged --experimental-scene --frontend-world
```

This reads the original compressed world files and selects an authored frontend
camera. Unsupported object types are reported; animated objects, effects and
menu behavior remain pending. Use `--debug-camera` to inspect it freely.

Add `--frontend-layout /Art/fe/main_menu_v3.fen` to inspect stored text components
using the game's font textures and your configured USA text language. Up/Down
or a controller selects text through the original frontend input code. This is
a text asset viewer; animation and menu actions are still pending.
It uses separate controls from `--debug-camera`.
Supported plain text follows the original font measurement, kerning and glyph
draw order. Font descriptions and texture pages load through staged NL reads;
failed or cancelled loads leave the current scene intact.
Both frontend viewers register retained textures and submit original polygon
packets, preserving the authored order of mixed text and images.

For a stored image/text layout, use
`--frontend-frame /Art/fe/game_summary.fen --frontend-slide Slide1 --frontend-images ingame`
instead. This loads the original in-game image bundles and renders supported
static components in their authored order. Add `--frontend-animate` to play
supported authored animation tracks, with pause/reset controls in the preview.
Animated previews also offer original presentation/component slide selection.
Left/Right or a controller selects presentation slides through the original
frontend input code, unless the debug camera is active. The bounded native
handler uses original base update rules; game menu actions remain pending.
`--frontend-pointer Layer/Item` inspects original pointer events for a rendered
image/text instance. It uses the actual presented viewport and respects UI capture
and focus changes; concrete menu actions remain in development.
Instance inspection finds named component paths such as `Layer/Item` and changes
visibility, position and colour through the original setter rules. These edits
affect only the preview; animation can update them and Reload restores the file.
Reload keeps the current scene active until all replacement resources are ready;
pending reloads can be cancelled. Bounded runs use a fixed 60 Hz timeline.
Other frontend layouts default to the
`main` image bundle context; missing textures report an error. Concrete game handlers,
menu actions, movies, clipping and unsupported text formats remain pending.

Press Escape or close the window to exit. Add `--frames 180` for a bounded run,
`--unlit` to compare lighting, or use `--help` for other preview options. Running
`./build/scene/mscharged` without arguments opens the launcher.

To exercise the retail boot screen, use `--experimental-scene --frontend-boot`.
It loads the original startup assets and runs the boot handler's timing and
input. Enter or controller A dismisses the strap after its minimum delay.
The strap, Nunchuk, ESRB and logo screens render their authored content, including
the original logo sound. The final loading screen waits for the remaining game
startup services; the main menu is still in progress. Bounded runs (`--frames N`)
use neutral input to exercise automatic dismissal.

For a camera-only NIS picture-in-picture preview, add `--nis-primary /DISC/first.nis`
and `--nis-secondary /DISC/second.nis`. Each selects its first embedded camera.
The preview offers PIP, camera swap and expansion controls; `--pip-expand 1`
starts a one-second expansion. Geometry remains static; NIS actors, audio and
scripted effects are not enabled.
