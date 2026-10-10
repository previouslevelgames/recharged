# Runtime

The Release build runs the whole original game on Linux/Vulkan; macOS/Metal is
being tested and Windows is in progress. It requires a USA `R4QE01` revision 1
ISO/RVZ. See [Building](BUILDING.md) for the Release build and disc
configuration, and the [feature list](FEATURES.md) for what is supported.

## Graphics prerequisites

Linux uses Vulkan; macOS selects Metal and remains under runtime validation.
Install the [build prerequisites](BUILDING.md#requirements), then initialize the
required pinned sources from the repository root:

```sh
python3 tools/setup_dependencies.py
```

The first graphics build is large. Adjust parallelism for available memory.
Real GPU tests are opt-in; the Vulkan test gate requires Linux and installed
validation layers. Windows and other disc regions remain unsupported.

## Original frontend sequence

```sh
CMAKE_BUILD_PARALLEL_LEVEL=3 cmake --workflow --preset frontend
./build/graphics/mscharged --disc ./game/R4QE01.rvz --window
```

Release includes the same source cohort at `build/release`; `frontend` keeps
its existing `build/graphics` location. Both workflows build only `mscharged`.
The runtime runs original `main`, loading,
frontend world setup, Boot/Intro, Title and Main Menu, including the initial save prompt.
An explicit `--experimental-frontend` remains available, including for INI-only startup.
**Menus and gameplay are still being integrated; this test can stop at
unfinished host services.** Audio quality and performance remain in progress.

Press **Enter or Space** (Wii A) to skip the intro movie through the original
handler. Use the mouse pointer and Enter/Space to select menu items; Escape/Backspace
is Wii B. The keyboard Wii Remote has a Nunchuk attached: W/A/S/D move its
stick and C/V are its C/Z buttons. E shakes the Remote and Q the Nunchuk (in a
match: hit, and switch items). P saves the presented frame to
`screenshots/screenshot_<timestamp>.png` in the working directory. Keep the game window focused. The launcher's **Play** button starts
the same runtime. Close the window to exit; omitting `--window` runs a bounded test.
The separate `--experimental-options` shortcut requires a build without the
original Boot script.

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

## Wii Remote

Experimental, Linux. Wii Remotes (or Wii Remote Plus) with a Nunchuk play
through a Mayflash DolphinBar in mode 4, which is also the sensor bar for the
Remote's pointer. Pair each Remote with the bar (SYNC on the bar, then SYNC
under the Remote's battery cover); players follow the bar's slot order. A Remote
paired directly over Bluetooth uses the same driver but is not verified yet. The
Remote speaker, MotionPlus and other extensions are not supported.

Choose who plays on the launcher's Controls page: each of players 1-4 is
keyboard & mouse, a Wii Remote connected to the bar, or off (`controls.player1`
to `player4`: `keyboard`, `remote1`-`remote4`, `off`). Players fill in order;
without a DolphinBar, keyboard & mouse is player 1 alone. The page also shows
each Remote's battery level. `controls.sensor_bar` is `bottom` or `top` of the
screen, and `controls.rumble` switches rumble. The bar must be in view of the
Remote's camera: sit about 1 m or more away when it is on top of the screen.
To make the pointer land where you aim, press the target button next to a
Remote and aim at the five targets that follow, pressing A at each (B cancels).

If no Remote is found, allow your user to open Wii Remotes once, then reconnect
the bar or Remote:

```sh
sudo tee /etc/udev/rules.d/60-mscharged-wiimote.rules >/dev/null <<'EOF'
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", KERNELS=="*057E:0306*", TAG+="uaccess"
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", KERNELS=="*057E:0330*", TAG+="uaccess"
EOF
sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=hidraw
```

Started from a terminal, the game logs `Wii Remote connected via DolphinBar slot N`;
`MSCHARGED_WIIMOTE_DEBUG=1` logs every step of the Remote setup.

## Performance log

`MSCHARGED_FRAME_LOG=frames.csv` writes one row per original game frame (frame
time, game-thread and process CPU, VI fields, presents, draws, pipeline creations)
and, on exit, `frames.csv.summary.txt` with the frame-time distribution and the
longest frames.

## Run options

Both tests accept `--config FILE`, `--disc FILE` (also `--disk`), `--window`,
`--fullscreen`, `--size WIDTHxHEIGHT` and `--aspect auto|4:3|16:9`. These override
INI settings for this run without saving. The default layout is 16:9; resizing
preserves the selected layout and fills unused space with bars. The launcher also
saves the start display, VSync, frame rate in the title, speaker volume, graphics
validation and log detail in `mscharged.ini`.

## Other checks

For the original initialization boundary without graphics:

```sh
python3 tools/setup_dependencies.py --launcher
git -c submodule.recurse=false submodule update --init --checkout -- extern/aurora extern/abseil-cpp extern/fmt extern/xxhash extern/tracy extern/zlib-ng
cmake --workflow --preset startup
./build/startup/mscharged --experimental-startup --config ./mscharged.ini
```

This stops explicitly at the unfinished startup boundary with exit code 3.
For rendering without game data, build preset `graphics` and run
`./build/graphics/mscharged-gx-check --window`.
