# Experimental runtime

The port is a work in progress alongside the decompilation. Selected tests run
original game code on Linux/Vulkan; **full startup and matches are unfinished**.
These tests currently require a USA `R4QE01` revision 1 ISO/RVZ. See
[Building](BUILDING.md) for the Release runtime build and disc configuration.

## Graphics prerequisites

Linux uses Vulkan; macOS selects Metal and remains under runtime validation.
Install the [build prerequisites](BUILDING.md#requirements), then initialize the
required pinned sources from the repository root:

```sh
python3 tools/setup_dependencies.py
```

The first graphics build is large. Adjust parallelism for available memory.
Real GPU tests are opt-in; the Vulkan test gate requires Linux and installed
validation layers. Windows, other disc regions and Wii peripherals remain unverified.

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

Experimental, Linux. A Wii Remote (or Wii Remote Plus) with a Nunchuk plays
alongside the keyboard; the mouse stays the pointer (sensor-bar IR, the Remote
speaker and other extensions are not supported yet).

Once, allow your user to open Wii Remotes, then reconnect the Remote:

```sh
sudo tee /etc/udev/rules.d/60-mscharged-wiimote.rules >/dev/null <<'EOF'
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", KERNELS=="*057E:0306*", TAG+="uaccess"
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", KERNELS=="*057E:0330*", TAG+="uaccess"
EOF
sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=hidraw
```

Pair each Remote once with its Nunchuk plugged in: in the Bluetooth settings add
a device and press the red **SYNC** button under the battery cover (not 1+2), then
choose *Nintendo RVL-CNT-01*. Connect the Remotes before starting the game, keep
their Nunchuks plugged in, and restart the game if a Remote disconnects. Hold the
Remote upright and level while pointing with the mouse. `controls.rumble` in
`mscharged.ini` switches rumble. Started from a terminal, the game logs
`Wii Remote connected on WPAD channel N`, or the reason it could not use a Remote.

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
