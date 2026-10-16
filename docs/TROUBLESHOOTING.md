# Troubleshooting

Solutions to the problems that come up most. If yours is not here, open an
[issue](https://github.com/yannicksuter/mscharged-port/issues) and attach
`logs/mscharged.log` from the program folder (see
[reporting a problem](#reporting-a-problem)). Steam Deck specifics are at the
end of the [Steam Deck guide](STEAM_DECK.md#troubleshooting).

## The launcher does not accept my disc

The port runs **Mario Strikers Charged for the USA, game ID `R4QE01`,
revision 1**. The launcher's **Game** page shows the ID and revision of the
image you chose.

- **`R4QP01` (Europe):** supported from version 1.0.10 on (revisions 1 and
  2).
- **Another ID**, such as `R4QJ01` (Japan): not supported yet; the
  decompilation of these versions is still being completed. Renaming the file
  does not change the version inside it.
- **"This build accepts ISO and RVZ disc images":** only complete `.iso` and
  `.rvz` images work; other formats (`.wbfs`, `.ciso`, `.wia`, `.gcz`) and
  extracted disc folders are not supported.
- **"Expected a Mario Strikers Charged Wii image":** the file is another game
  or not a Wii disc image.
- The image must stay where you chose it: the port reads it every time you
  play.

## The first start takes a while

The first start shows **Preparing graphics** while the shaders for your
graphics card are compiled. This runs once (and again after a graphics driver
update); later starts load them from the cache within seconds.

## A short stutter during play

A stutter the first time an effect appears usually means a shader was missing
from the package's list and had to be compiled on the spot. It is compiled
only once per PC. The log records it as
`Graphics: new pipeline compiled while drawing`; attaching that log to an
issue gets it added to the package, so nobody else sees the stutter.

## Windows: SmartScreen blocks the program

The builds are not signed. Choose *More info → Run anyway*. Antivirus programs
can also quarantine new unsigned programs; restore the file and allow it.

## macOS: the app cannot be opened

The builds are not signed. Allow the app under *System Settings → Privacy &
Security* (*Open Anyway*), or run in Terminal:

```sh
xattr -dr com.apple.quarantine <unpacked folder>
```

## macOS: controllers do nothing

macOS only lets an app read gamepads and Wii Remotes after you allow **Input
Monitoring** for it, in *System Settings → Privacy & Security* (for the app
you start it from, such as Terminal, if you use one). Quit and restart the
game afterwards, then unplug and reconnect the controller: one that was
already connected stays silent until it is opened again.

## Linux: the program does not start

- Mark it executable: `chmod +x mscharged`.
- An error about `GLIBC_2.43`, `GLIBCXX` or `CXXABI` means your distribution
  is older than the build machine (Ubuntu 26.04). Use the
  `steamdeck-x86_64` package instead: the same build, made with older system
  libraries (glibc 2.39 and the GCC 14 runtime, as in Ubuntu 24.04 or
  SteamOS 3.7).
- A message about Vulkan means the graphics driver lacks Vulkan support:
  install Mesa's Vulkan driver for your GPU (`mesa-vulkan-drivers`,
  `vulkan-radeon`, `vulkan-intel` or similar) or your GPU vendor's driver.

## A gamepad does nothing

- On the launcher's **Controls** page, assign the gamepad to a player
  (`gamepad1` is the first connected one). Players fill in order: player 1
  must be set before player 2 plays.
- The **Controller test** card on that page shows whether the buttons arrive.
- Controllers that are missing when the game starts are left out; connect them
  before pressing Play.
- On a Steam Deck, or with Steam running, Steam Input must present the
  controller as a gamepad.

## A Wii Remote is not found

See [Wii Remotes](SETTINGS.md#wii-remotes): the DolphinBar must be in mode 4,
and on Linux the udev rule there must be installed once. Over Bluetooth the
Remote needs a sensor bar for its pointer. The Controls page shows each
connected Remote and its battery.

## No sound

The game plays through your system's default audio device. Check
**Audio → Mute** and **Master volume** in the launcher, and the game's own
music, effects and voice levels in its *Options* menu.

## Resetting the settings

**Advanced → Reset → Restore defaults** resets every setting except your disc.
To start completely fresh, quit and delete `mscharged.ini` from the program
folder. Saves are separate and stay.

## Reporting a problem

Open an [issue](https://github.com/yannicksuter/mscharged-port/issues) with:

- the version (launcher **About** page, or the first line of the log),
- your system and graphics card,
- the controllers you used, and what you did when the problem appeared,
- `logs/mscharged.log` from the program folder (it is replaced on the next
  start, so copy it first). On Windows a crash also leaves
  `mscharged-crash.dmp` there; attach it too.

The log can contain your user name in file paths.
