# Settings

Everything the launcher sets is saved in `recharged.ini` beside the program;
[`recharged.ini.example`](../recharged.ini.example) explains every key. This
page covers the settings that need more than a label: game images, graphics,
audio, Wii Remotes, saves, updates and command-line options. For installing, see the
[install guide](INSTALL.md); for the full list of features and controls, the
[feature list](FEATURES.md).

## What is stored where

Everything stays in the program folder:

| Path | Contents |
| --- | --- |
| `recharged.ini` | Your settings (older versions: `mscharged.ini`, copied at the first start) |
| `data/nand/` | The game's Wii saves (or the folder set under *Game → Save data*) |
| `data/pipeline_cache.db` | Compiled shaders for your graphics card |
| `data/launcher/` | What the launcher found in your game images, and their banners |
| `logs/recharged.log` | The log of the last run |
| `screenshots/` | Screenshots taken with **P**, in the folder the game was started from |

Updates replace only the program's own files; these stay. If the program
folder is not writable, the log goes to your user's application data folder
instead.

## Game images

*Game → Game images* is one table of the Mario Strikers Charged images
(`.iso`, `.rvz`, `.wbfs`) in your game folder: title, region, revision, game
ID, format, size and file name. Click a column title to sort by it.

**Choosing the image.** Click an image to play it: its row is marked, and Play
starts it from then on (`[game] disc`, saved with your settings). Sorting or
refreshing never changes it. **Choose file...** (or dropping an image onto the
launcher) plays an image from outside your game folder; it stays chosen until
you click another image, whatever folder you select. Images of versions this
build cannot run yet are listed with the reason and cannot be chosen. If the
chosen image moves or its drive is not connected, its row and the Play page say
so; click the row (or **Locate...**) to find it, use a copy of the same
release, or choose another image. Recharged never switches to another image by
itself.

**Game folder.** By default the `game` folder beside the program; clicking it
(or **Change...**, or dropping a folder onto the launcher) selects another one,
and *Subfolders* also lists the images up to four levels below it. The list
always shows the folder as it is now: it is read again at every start, when you
select a folder or switch *Subfolders*, and with the refresh button. When the
new folder or setting leaves out the chosen image, the folder's first playable
image takes its place, or none when the folder has none. A folder on a drive or
memory card that is not connected shows *Not found* until it is. Images of
other games are left out.

**Banners.** The small horizontal pictures are the banners the game puts into
its Wii save, one per region. The launcher reads each one from your own image
and keeps a copy in `data/launcher/banners/`; Recharged ships no game artwork
and downloads nothing.

The folder is stored as `[game] folder` and `folder_subfolders` in
`recharged.ini`; a relative folder counts from the settings file, like the
disc.

## Graphics

**Graphics API.** Linux uses Vulkan and macOS uses Metal. On Windows,
**Advanced → Graphics API** chooses Vulkan or DirectX 12 for the next start
(`[display] backend = vulkan` or `d3d12`; `auto` means Vulkan). If the chosen
API fails, the game reports an error instead of silently switching. Keep the
shader compiler DLLs from the Windows package beside `recharged.exe`.

**Preparing graphics.** Before the game starts, Recharged builds the game's
drawing and effect pipelines for your graphics card: the ones listed in the
package's `assets/graphics/initial_pipeline_cache.db`, recorded from runs
through every stadium and captain, plus any others the game has used on this
PC. The first start compiles them all once, on several processor cores; later
starts load them from the cache. A new graphics driver can trigger a new
compile. Nothing is prepared during play: a pipeline missing from the list
compiles when it is first drawn, which can cause one short stutter, and is
prepared from the next start on. The log records each one
(`Graphics: new pipeline compiled while drawing in … ms`); attaching such a
log to an issue gets the pipeline added to the package for everyone.

**Clear shader cache** (Advanced → Diagnostics & analytics) removes the compiled
shaders and the pipelines learned on this PC. The next start prepares the
bundled list again. Saves, settings and frame recordings are kept.

**Frame rate** (Display → Frame rate, `[display] frame_rate`) draws above the
console's 60 FPS: `display` follows the refresh rate of the window's display,
`unlimited` sets no limit, and a number from 30 to 1000 caps the rate. VSync
holds any rate to the display's. The game keeps its own speed; movies and
everything else timed by the Wii's video signal stay at their real rate. The
log's `Frame rate:` line shows how frames are paced.

**Picture.** *Sharp* (default) and *clean* fill the window with the game's
picture; *soft* keeps the look of the Wii's TV signal, with its empty border.
The 3D scene renders at the window's resolution by default, or at the Wii's
640 x 448 or 2x-4x of it (Display → 3D resolution, `[display] resolution`).
Antialiasing (4x) is on by default. The layout is 16:9 by default, or 4:3;
resizing keeps it and fills unused space with bars.

**Performance mode** (the gear on the **Play** button,
`[display] performance_mode`) lightens matches for weaker PCs: no 4x
antialiasing, crowd, background NPCs, ambient stadium effects, bloom, heat
distortion, depth of field or stadium shadows. Gameplay is untouched: hazards,
lightning, wind, debris, character and ball shadows stay, and so do the effects
that show them. It keeps your 3D render size, leaves the menus as they are, and
restores your antialiasing preference when turned off.

**Performance overlay** (Display) shows `FPS | GPU | CPU | LAT` in a corner of
your choice, also in fullscreen. GPU and CPU are total system usage, including
other apps; drivers that hide the GPU counters show `N/A`. LAT stays `N/A`
while there is no online play.

## Audio

**Master volume** and **Mute** (Audio → Output) set the game's whole output.
**Sound levels** (Audio, `[audio] music_volume` to `movie_volume`) set each kind
of sound from 0 to 100 % of the level the game gives it, for example to turn
the music down for a recording while voices and effects stay:

| Level | Sounds |
| --- | --- |
| Music | Menu and match music |
| Sound effects | Kicks, tackles, items, the electric fence, stadium and weather sounds |
| Voices | The characters' voice lines, also in the menus |
| Crowd | The stadium crowd's cheering and chants |
| Menus & HUD | Menu clicks and prompts, the studio logo's jingle, the sounds of the match's on-screen display |
| Cutscenes | Captain introductions, goal celebrations, Mega Strikes and the scenes after a match, with their own music and voices |
| Movies | The intro movie, the stadium previews and the credits |

They apply when the game starts. The music, sound effect and voice settings
in the game's own *Options* menu still apply on top and stay saved with your
game; cutscenes and movies have no setting there. The log's `Sound levels:`
line lists the levels in use.

## Wii Remotes

Real Wii Remotes (or Wii Remote Plus) with a Nunchuk work on Linux and
Windows, either through a **Mayflash DolphinBar** in mode 4 or paired directly
over **Bluetooth**. On macOS they are untested. The Remote speaker, MotionPlus
and other extensions are not supported.

- **DolphinBar:** press SYNC on the bar, then SYNC under the Remote's battery
  cover.
- **Bluetooth on Windows:** while the launcher's **Controls** page is open, it
  connects Remotes by itself: press 1+2 or the SYNC button on the Remote (some
  connect more easily with SYNC). `[controls] bluetooth_search = false` turns
  this off.
- **Bluetooth elsewhere:** press SYNC and connect "Nintendo RVL-CNT-01" in
  your system's Bluetooth settings (no PIN).

Over Bluetooth the Remote needs a separate sensor bar for its pointer.
`controls.sensor_bar` says whether the bar sits at the `bottom` or `top` of
the screen; it must be in view of the Remote's camera (sit about 1 m or more
away when it is on top). `controls.rumble` switches rumble.

**Who plays** is set on the **Controls** page: each of players 1-4 is keyboard
& mouse, a Wii Remote, a gamepad, or off (`controls.player1` to `player4`:
`keyboard`, `remote1`-`remote4` for DolphinBar slots, `remote-<address>` for a
Bluetooth Remote, `gamepad1`-`gamepad4`, `off`). Players fill in order.
Controllers missing when the game starts are left out, and keyboard & mouse is
player 1 when nobody is left. The page also shows each Remote's battery level.

**Linux permissions.** If a Remote is connected but cannot be opened, the
Controls page says so and copies these commands. Run them once, then
reconnect the bar or Remote:

```sh
sudo tee /etc/udev/rules.d/60-recharged-wiimote.rules >/dev/null <<'EOF'
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", KERNELS=="*057E:0306*", TAG+="uaccess"
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", KERNELS=="*057E:0330*", TAG+="uaccess"
EOF
sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=hidraw
```

For diagnosis, **Advanced → Verbose console** (or `RECHARGED_WIIMOTE_DEBUG=1`)
logs every step of the Remote setup, including the devices the launcher sees
and how each Remote answers.

## Saves

The game keeps its Wii saves (`Strikers2` and `banner.bin`) in `data/nand` in
the program folder, or in the folder set under **Game → Save data**
(`[game] save_folder`). A new folder starts without saves; the old folder
keeps its saves. Each version has its own saves, as on the Wii: the USA
version uses the folder itself, the European version its `R4QP` subfolder
(shared by both European revisions).

Earlier builds called the data folder `original-main-credits-data`; the first
start of a newer build renames it to `data`. Switched back to an earlier
build, the game starts without saves until `data` gets its old name again.

**Import from Dolphin** copies the save Dolphin keeps for the game
(`<Dolphin user folder>/Wii/title/00010000/52345145/data` for the USA version,
`.../52345150/data` for the European one) byte for byte. The
launcher finds Dolphin's usual folders by itself; otherwise choose Dolphin's
user folder or that save folder. The current save is first copied to
`backups/` in the save folder. Dolphin's encrypted `data.bin` exports are not
supported.

## Updates

At each start the launcher asks GitHub whether a newer build exists (one
anonymous request). When it finds one, it shows the release notes and asks
before installing. **Update now** downloads the package for your system,
checks its SHA-256 and every file, replaces only the program's own files and
restarts the launcher; if anything fails, the old files come back. Settings,
saves, caches and logs stay. A folder that was not unpacked from a package, or
is not writable, gets a download link instead.

**Advanced → Updates** turns the automatic check off
(`[launcher] check_updates`); a click on the update status on the Play page
checks on demand.

**Release / Test** on the Play page (`[launcher] update_channel`) chooses what
to follow: official releases, or also the **test builds** of changes that are
not released yet, with the change's description as notes. Switching asks
whether to install the other channel's build. On a test build, *Release*
offers the way back to the latest release. A test build moves on to the
release that contains it.

## Command-line options

The launcher is optional: starting `recharged` with a disc image goes straight
into the game, with the settings from `recharged.ini`:

```sh
./recharged --disc "/path/to/Mario Strikers Charged.rvz"
```

These options override the INI for one run, without saving:

| Option | Effect |
| --- | --- |
| `--disc FILE` (or `--disk`) | Start the game with this disc image |
| `--window`, `--fullscreen` | Window or fullscreen |
| `--size WIDTHxHEIGHT` | Window size, e.g. `--size 1280x720` |
| `--aspect auto\|4:3\|16:9` | Layout |
| `--frame-rate original\|display\|unlimited\|FPS` | Frame rate |
| `--no-tweaks` | All `[gameplay]` tweaks off (the bug fixes stay) |
| `--config FILE` | Use another INI file |
| `--launcher` | Open the launcher even with `--disc` |

`recharged --help` lists them all.

## Logs and frame recordings

Each run writes `logs/recharged.log` beside the program: settings, devices,
graphics adapter, warnings and errors. Attach it to an
[issue](https://github.com/previouslevelgames/recharged/issues) when reporting a
problem; it can contain your user name in paths. **Advanced → Log detail** and
**Verbose console** add more detail.

On Windows and macOS no console or Terminal window opens with the game.
**Advanced → Diagnostics & analytics → Log window** (`[advanced] log_window`,
off by default) shows the log as it is written, in a separate window (Terminal
on macOS). Closing that window or selecting text in it does not stop or slow
the game; it closes when the game ends (on Windows it stays open after an
error until you press a key). Started from cmd or PowerShell, `recharged.exe`
also prints to that console, whose prompt returns at once
(`start /wait recharged.exe` waits for the game). On macOS the `recharged`
file beside the app runs it in Terminal with the output there.

**Advanced → Diagnostics & analytics → Performance stats**
(`[advanced] frame_logging`) records frame times: each session writes a
`frame-times_<timestamp>_<process>.csv` in the `logs` folder beside the program
(next to `recharged.log`), with one row per game frame (frame time, game-thread
and process CPU, presents, draws, pipeline creations), and a `.csv.summary.txt`
with the distribution and longest frames on exit. Each row also names the arena
being played, and the summary starts with the version and settings, then
repeats the figures for every arena, so testers can compare arenas. **Frame
recordings → Open folder** opens that location.
