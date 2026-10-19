# Settings

Everything the launcher sets is saved in `recharged.ini` beside the program;
[`recharged.ini.example`](../recharged.ini.example) explains every key. This
page covers the settings that need more than a label: game images, graphics,
audio, Wii Remotes, saves, texture packs, updates and command-line options. For installing, see the
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
| `texture_packs/` | Texture packs (or the folder set under *Gameplay → Mods*) |
| `data/texture_dumps/` | The game's textures, when *Dump textures* is on |
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
**Advanced → Graphics API** chooses Automatic, Vulkan or DirectX 12 for the next
start (`[display] backend = auto`, `vulkan` or `d3d12`). Automatic uses Vulkan
and switches to DirectX 12 when Vulkan cannot start, for example when the
graphics driver is too old for it. A chosen API is never switched: if it fails,
the game says so. Keep the shader compiler DLLs from the Windows package beside
`recharged.exe`.

**Graphics card** (Windows and Linux, **Advanced → Graphics card**,
`[display] gpu`). *Automatic* draws with the fastest card; on a PC with two
(a gaming laptop, say) you can choose one. The launcher lists the cards
Windows offers (on Linux, those with a Vulkan driver) and stores the choice as
the card's PCI ID, such as `10de:1f99`. A card switched off in Device Manager or
by a laptop's GPU mode is not listed, and a chosen card that the graphics API
does not offer falls back to *Automatic*.

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
The layout is 16:9 by default, or 4:3; resizing keeps it and fills unused
space with bars.

**Fullscreen** (Display → Mode, `[display] fullscreen`) covers the whole
display at its own resolution. **Alt+Enter** (Option+Return on a Mac) switches
between fullscreen and a window while playing, for that run only; that Enter
never presses A.

**Antialiasing** (Display → Antialiasing, `[display] antialiasing`) smooths
jagged edges. **4x MSAA** (`4x`, the default) smooths the edges of 3D shapes.
**FXAA** (`fxaa`) smooths the finished picture at almost no cost, but softens
fine detail and text a little. **4x MSAA + FXAA** (`4x_fxaa`) does both.
**4x SSAA** (`4x_ssaa`) shades each of the 4 samples of a pixel, which also
smooths the edges inside textures, such as stripes on banners; it costs the most.
**Off** (`off`) is the lightest. Multisampling always uses 4 samples: the
graphics layer offers no 2x or 8x.

**Performance mode** (the gear on the **Play** button,
`[display] performance_mode`) lightens matches for weaker PCs: no
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
over **Bluetooth**. On macOS they are untested. MotionPlus and other
extensions are not supported.

- **DolphinBar:** press SYNC on the bar, then SYNC under the Remote's battery
  cover.
- **Bluetooth on Windows:** while the launcher's **Controls** page is open, it
  connects Remotes by itself: press 1+2 or the SYNC button on the Remote (some
  connect more easily with SYNC). `[controls] bluetooth_search = false` turns
  this off.
- **Bluetooth elsewhere:** press SYNC and connect "Nintendo RVL-CNT-01" in
  your system's Bluetooth settings (no PIN).

Over Bluetooth the Remote needs a separate sensor bar for its pointer.
The **Wii Remote** card on the Controls page has the Wii's own Remote
settings:

- **Sensor bar** (`controls.sensor_bar`): whether the bar sits at the
  `bottom` or `top` of the screen. It must be in view of the Remote's camera
  (sit about 1 m or more away when it is on top).
- **Sensitivity** (`controls.ir_sensitivity`, 1-5, default 3): how readily
  the camera sees the bar, as in the Wii's sensor bar settings. Raise it when
  the pointer drops out far from the bar, lower it when lamps or windows
  disturb it.
- **Speaker volume** (`controls.speaker_volume`, 0-100, default 70 like the
  Wii): the game plays some sounds on each player's Remote as well as on the
  TV. 0 sends no sound to the Remotes. How clean it sounds depends on the
  Bluetooth link; turn it down or off if it crackles. The log
  (`logs/recharged.log`) says when the connection was too slow for it.
- **Speaker format** (`controls.speaker_format`, INI only): `pcm` (default)
  sends the Remotes 8-bit sound at 3000 samples a second, made from the
  game's own speaker sound. `adpcm` sends the game's sound as a Wii does
  (6000 samples a second). A Wii runs its Remote link in a faster mode that
  PC Bluetooth cannot switch on, so a Remote can miss some of the game's
  speaker data: with `adpcm` that turns the rest into noise until the next
  pause, with `pcm` it only skips a moment. `pcm` sounds duller; try `adpcm`
  if your Remote plays it cleanly.

The **Rumble** card switches rumble for Wii Remotes and gamepads
(`controls.rumble`) and sets how strongly gamepads rumble
(`controls.rumble_strength`, 0-100). A Wii Remote's motor is either on or
off.

**Who plays** is set on the **Controls** page: each of players 1-4 is keyboard
& mouse, a Wii Remote, a gamepad, or off (`controls.player1` to `player4`:
`keyboard`, `remote1`-`remote4` for DolphinBar slots, `remote-<address>` for a
Bluetooth Remote, `gamepad1`-`gamepad4`, `off`). Players fill in order.
Controllers missing when the game starts are left out, and keyboard & mouse is
player 1 when nobody is left. The page also shows each Remote's battery level.

**Keys and gamepad profiles.** The sliders button beside a player opens its
keys (`[keyboard]`) or its gamepad profile (`[gamepad_profileN]`). Each action
takes up to two keys; the middle and side mouse buttons (`Mouse Middle`,
`Mouse 4`, `Mouse 5`) can take a key's place, while left and right click stay
A and B. A gamepad profile also sets, for both sticks:

- **Inner dead zone** (`inner_deadzone`, 0-30 %, default 15): the stick rests
  until it is tilted this far. Raise it if a worn stick drifts.
- **Outer dead zone** (`outer_deadzone`, 70-100 %, default 71): from this tilt
  on the stick is pushed all the way. The defaults play like a Nunchuk on the
  Wii; the game's own Nunchuk dead zone is already included, so these are the
  only ones you feel.
- **Trigger press point** (`trigger_press`, 10-90 %, default 50): a trigger
  bound to a button presses it past this much of its travel.

**Sticks as buttons**: pushing a stick one way past half its travel can press
any action, like a button (`rightstick_up`, `leftstick_left` and so on in the
INI). To bind one, select the action and push the stick. With the gyro
pointer on, **Right stick as D-pad** puts the free stick on the D-pad: in a
match the D-pad dekes with the ball and slide-tackles without it. A stick that
also moves the Nunchuk stick or the pointer does both at once; the profile page
says so.

**Gyro pointer** (`gyro_pointer`, off by default): a controller with a gyro,
such as a PlayStation or Switch Pro controller, points by turning it, like a
Wii Remote, instead of with the stick. **Gyro speed** (`gyro_speed`, 25-300 %,
default 100): at 100 %, turning the controller about 25 degrees moves the
pointer across the picture. **Recenter pointer** (`recenter_pointer`, default
the right stick click) puts it back in the middle; it also goes there whenever
the game turns the pointer on, as at the start of a goalie's Mega Strike
defence. A controller without a gyro points with its stick. Controllers seen
through Steam Input usually show the game no gyro.

The profile page shows both sticks and triggers live with these settings, and
the gyro pointer when it is on.
The built-in Default always uses the defaults; changing it makes your own copy.

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

## Texture packs

Texture packs replace the game's textures with sharper images; they change
only how the game looks. Recharged reads packs made for Dolphin: a pack is a
folder or a `.zip` file of images named `tex1_<size>_<hash>_<format>.png` (or
`.dds`), usually in a folder named `R4Q` (all versions) or after one version
(`R4QE01`, `R4QP01`). Drop the pack's folder or `.zip` file onto **Gameplay →
Mods**, which copies it into `texture_packs` in the program folder (`[mods]
texture_folder`; you can choose another folder there), or put it there
yourself. The list there switches packs on and off; drag a pack (or use its
menu) to change the order: where two packs replace the same texture, the
higher one wins. A Dolphin `Load/Textures` folder works as the packs folder too.

Images of the pack load in the background while the game runs: a texture can
show its original look for a moment before its replacement appears. Packs with
`.dds` images (BC1, BC3 or BC7) load faster and use less graphics memory than
`.png` ones. The images match the game's textures by their contents, so
textures with text need images for each language. Mirror match's away kits
recolour the pack's image of the home kit (in a picture the pack drew anew, its
kit is found by colour); the Super Team keeps its own look.

**Dump textures** (`[mods] dump_textures`) is for pack makers: each
texture no pack replaces is saved to `data/texture_dumps` as a `.dds` file
named as packs need it.

## Discord status

While the Discord app runs on the same computer, your Discord profile shows
what you are playing: the menu you are in, or the match with its mode,
stadium, captains, score and match time. The game talks only to the Discord
app on your computer, which shows the status to your friends. Switch it off on
the launcher's **Game** page (`[discord] rich_presence`); Discord's own
activity privacy setting hides it too. Builds that do not come from the
project's releases and test builds have no Discord application and show no
status, unless `[discord] application_id` names one.

## The launcher

**Reopen after the game** (Advanced → Interface, `[launcher] reopen_after_game`,
off by default) opens the launcher again when you close the game. A game that
stops with an error message closes as before.

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
whether to install the other channel's build; *Later* keeps the build and
the choice you had. On a test build, *Release*
offers the way back to the latest release. A test build moves on to the
release that contains it.

Several changes can be tested at once, such as the next bug-fix release and a
bigger feature release. Then **Test** gets a menu (click it again) listing the
test build of each, newest version first; choose one to read its notes and
switch to it (`[launcher] test_build`). You keep getting the updates of the
test build you chose, then its release; the launcher never moves you to
another one by itself.

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
