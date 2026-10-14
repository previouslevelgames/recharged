# Runtime

The Release build runs the whole original game on Linux/Vulkan; macOS/Metal is
being tested and Windows is in progress. It requires a USA `R4QE01` revision 1
ISO/RVZ. See [Building](BUILDING.md) for the Release build and disc
configuration, and the [feature list](FEATURES.md) for what is supported.

## Graphics prerequisites

Linux uses Vulkan; macOS selects Metal and remains under runtime validation.
On Windows, **Advanced → Graphics API** selects Vulkan or DirectX 12 for the
next game launch (`display.backend = vulkan` or `d3d12` in the INI). Existing
`auto` settings keep Vulkan. A failed selection reports an error without
switching APIs. Keep the three shader compiler DLLs included in the Windows ZIP
beside `mscharged.exe`.

Before the game starts, **Preparing graphics** builds the game's drawing and
effect pipelines for the selected graphics API and GPU: the ones listed in the
package's `assets/graphics/initial_pipeline_cache.db`, recorded from runs through
every stadium and captain, and any others the game has used on this PC. The
first launch compiles them all once, on several processor cores; later launches
load them from the local cache. Nothing is prepared once the game runs: a
pipeline missing from both compiles when it is first drawn, which can cause a
brief stutter, and is prepared from the next launch on. Driver, GPU or renderer
changes may require compilation again. Keep `assets/graphics/initial_pipeline_cache.db`
in the package.

Each graphics API retains its own compatible compiled data across launches,
including when switching away from that API and back. The session log records
the preparation time and pipeline cache hits, misses and time spent resolving
misses.

Install the [build prerequisites](BUILDING.md#requirements), then initialize the
required pinned sources from the repository root:

```sh
python3 tools/setup_dependencies.py
```

The first graphics build is large. Adjust parallelism for available memory.
Real GPU tests are opt-in; the Vulkan test gate requires Linux and installed
validation layers. DirectX 12 rendering and performance still need validation
on real Windows hardware. Other disc regions remain unsupported.

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
the same runtime. Close the window to exit, also in fullscreen: Alt+F4 on Windows
and most Linux desktops, Cmd+Q on macOS. The game then runs its own shutdown, like
the Wii power button, and quits after a short fade. Omitting `--window` runs a bounded test.
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

Experimental: tested on Linux, connecting over Bluetooth also on Windows. Wii Remotes (or Wii Remote Plus) with a Nunchuk play
either through a Mayflash DolphinBar in mode 4 or paired directly over
Bluetooth. Pair with the bar by pressing SYNC on the bar, then SYNC under the
Remote's battery cover. On Windows, the launcher's Controls page connects
Remotes over Bluetooth while it is open: press 1+2 or the SYNC button on the
Remote (some connect more easily with SYNC); `controls.bluetooth_search`
switches this off. Elsewhere, press SYNC and connect "Nintendo RVL-CNT-01" in
your system's Bluetooth settings (no PIN). Over Bluetooth the Remote needs a separate sensor bar for its
pointer. The Remote speaker, MotionPlus and other extensions are not supported.

Choose who plays on the launcher's Controls page. Each of players 1-4 is
keyboard & mouse, a connected Wii Remote, a gamepad, or off (`controls.player1`
to `player4`: `keyboard`, `remote1`-`remote4` for DolphinBar slots,
`remote-<address>` for a Bluetooth Remote, `gamepad1`-`gamepad4`, `off`).
Players fill in order. Controllers missing when the game starts are left out,
and keyboard & mouse is player 1 when nobody is left. The page also shows
each Remote's battery level. `controls.sensor_bar` is `bottom` or `top` of the
screen, and `controls.rumble` switches rumble. The bar must be in view of the
Remote's camera: sit about 1 m or more away when it is on top of the screen.

If a Remote is connected but cannot be opened, the Controls page says so and
copies these commands. Run them once, then reconnect the bar or Remote:

```sh
sudo tee /etc/udev/rules.d/60-mscharged-wiimote.rules >/dev/null <<'EOF'
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", KERNELS=="*057E:0306*", TAG+="uaccess"
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", KERNELS=="*057E:0330*", TAG+="uaccess"
EOF
sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=hidraw
```

Started from a terminal, the game logs `Wii Remote connected via ...` with
the DolphinBar slot or Bluetooth address; **Advanced → Verbose console** or
`MSCHARGED_WIIMOTE_DEBUG=1` logs every step of the Remote setup, including the
HID devices the launcher sees and how each Remote answers (also in
`logs/mscharged.log`).

## Steam Deck

Use the `steamdeck-x86_64` package (SteamOS 3.7 or newer).

1. In Desktop Mode, unpack it to a folder you can write to, such as
   `~/Games/mscharged`, and start `mscharged`. Choose the disc on the
   **Game** page; on **Controls**, set player 1 to `gamepad1` (Steam presents
   the Deck's buttons and sticks as a gamepad).
2. In Steam, choose *Games → Add a Non-Steam Game to My Library* and add
   `mscharged` from that folder.
3. Start it from the library in Game Mode and press **Play** in the launcher.

In Game Mode (gamescope) the game opens fullscreen; `--window` in the
shortcut's launch options keeps a window. The Steam Deck build has not been
tested on a Deck yet.

## Saves

The game keeps its Wii saves (`Strikers2` and `banner.bin`) in
`original-main-credits-data/nand` next to the program, or in the folder set as
`[game] save_folder` (launcher: **Game → Save data**). A new folder starts
without saves; the old folder keeps its saves.

**Import from Dolphin** copies the save Dolphin keeps for the game
(`<Dolphin user folder>/Wii/title/00010000/52345145/data`) byte for byte. The
launcher finds Dolphin's usual folders on its own; otherwise choose Dolphin's
user folder or that save folder. The current save is first copied to `backups/`
in the save folder. Dolphin's encrypted `data.bin` exports are not supported.

## Performance log

Enable **Advanced → Diagnostics & analytics → Frame logging** to record frame
times (`advanced.frame_logging = true` in the INI; off by default). Each game
session writes a new `frame-times_<timestamp>_<process>.csv` directly beside the
executable, with one row per original game frame: frame time, game-thread and
process CPU, VI fields, presents, draws and pipeline creations. On exit, an
adjacent `.csv.summary.txt` reports the distribution and longest frames.
**Frame recordings → Open folder** opens that location. It must be writable;
logging failures are reported in the session log. The old `MSCHARGED_FRAME_LOG`
environment variable no longer enables logging or selects its output path.

The same card includes **Clear shader cache**, which removes compiled shaders and
the pipelines learned on this PC for all graphics APIs. The next launch prepares
the bundled pipelines again.
Clearing is refused while a game is using the cache. Saves, settings and frame
recordings are kept.

## Run options

Both tests accept `--config FILE`, `--disc FILE` (also `--disk`), `--window`,
`--fullscreen`, `--size WIDTHxHEIGHT`, `--aspect auto|4:3|16:9` and `--no-tweaks`
(the `[gameplay]` tweaks off). These override INI settings for this run without saving. The default layout is 16:9; resizing
preserves the selected layout and fills unused space with bars. The launcher also
saves the start display, VSync, the performance overlay, speaker volume,
graphics validation and log detail in `mscharged.ini`.

**Display → Performance overlay** shows `FPS | GPU | CPU | LAT` in the selected
corner, including in fullscreen. GPU and CPU show total system usage, including
other apps, sampled twice per second. GPU shows the busiest reported device or
engine; drivers or permissions that hide usage counters show `N/A`. LAT stays
`N/A` until networking is available. The window title contains no FPS counter.

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
