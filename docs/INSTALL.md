# Install and play

Ready-made packages for Windows, Linux, Steam Deck and macOS. You do not need
to build anything. On a Steam Deck, follow the [Steam Deck guide](STEAM_DECK.md)
instead.

> **The packages contain no game files.** You need your own copy of
> **Mario Strikers Charged** as an `.iso`, `.rvz` or `.wbfs` disc image: the
> USA version (game ID `R4QE01`, revision 1) or the European version (game ID
> `R4QP01`, revision 1 or 2). Other regions do not work yet, and renaming a
> file does not change that. An image patched for Wiimmfi plays too.

## 1. Download

Open the [latest release](https://github.com/previouslevelgames/recharged/releases/latest)
and download the package for your system:

| System | Package |
| --- | --- |
| Windows 10/11, 64-bit | `recharged-<version>-windows-x86_64.zip` |
| Linux, x86_64 | `recharged-<version>-linux-x86_64.tar.gz` |
| Steam Deck, SteamOS 3.7+ | `recharged-<version>-steamdeck-x86_64.tar.gz` |
| macOS 15+, Apple Silicon or Intel | `recharged-<version>-macos-universal.dmg` |

`SHA256SUMS` on the release page lists the checksums.

**Requirements:** a graphics driver with Vulkan (Windows, Linux), DirectX 12
(Windows) or Metal (macOS). The Mac disk image holds one app for Apple Silicon
and Intel Macs, with macOS 15 Sequoia or newer. The Linux package needs glibc
2.39 and the GCC 14 C++ runtime or newer, as in Ubuntu 24.04, Debian 13 or
Fedora 40 and later.

## 2. Install

- **Windows, Linux:** unpack the package into a folder of its own that you can
  write to, for example `Games/recharged` in your home folder.
- **macOS:** open the disk image and drag the **Recharged** folder onto
  **Applications** (or into any other folder you can write to). Copy the whole
  folder, not only `Recharged.app` inside it. Then eject the disk image.

Keep all files together: the program (`Recharged.app` on macOS), its two
modules and the `assets` folder. Settings, saves and logs are stored in this
folder too, so it can be anywhere but should stay in one place.

**Already playing on a Mac?** Keep your folder: the launcher updates it to the
new app. Do not replace it with the folder from a newer disk image, as that
would delete your settings and saves.

**Coming from an older version?** It called the program `mscharged`. Unpack
the new package over the old folder and start `recharged`: it takes over your
settings and saves. Then delete the old `mscharged` program and point any
shortcut (on the Steam Deck: *Target*) at `recharged`.

## 3. Start

Run **`recharged.exe`** (Windows), **`Recharged.app`** (macOS) or
**`recharged`** (Linux). The system asks once:

- **Windows:** the build is not signed. If SmartScreen warns, choose
  *More info → Run anyway*.
- **macOS:** start it the first time in Finder from the folder you copied:
  *Applications → Recharged → Recharged.app*, not from the disk image. From then
  on Spotlight or the Dock start it too. The app is signed and notarized by
  Apple, so macOS only asks whether to open an app downloaded from the internet:
  choose *Open*. Recharged may then ask once to *Allow and restart*: macOS
  started it from a temporary read-only copy, where it cannot see its folder.
  Recharged clears the download mark on its folder and starts again from there.
  Gamepads and Wii Remotes also need the **Input Monitoring** permission
  ([details](TROUBLESHOOTING.md#macos-controllers-do-nothing)). The `recharged`
  file beside the app starts it from Terminal.
- **Linux:** if the file will not start, mark it executable
  (`chmod +x recharged`).

## 4. Choose your game and play

The launcher opens. Put your `.iso`, `.rvz` or `.wbfs` into a `game` folder
beside the program, or on the **Game** page click the game folder
(**Change...**) to pick the folder that holds it, and click the image to play. **Choose file...** plays
an image from anywhere else. The image is read directly; do not extract or
mount it, and keep it where it is. Then press **Play**.

The first start shows **Preparing graphics** while the shaders for your
graphics card are compiled. This takes a little while once; later starts reuse
them.

On the **Controls** page assign keyboard & mouse, gamepads or Wii Remotes to
players 1-4 ([default controls](FEATURES.md#default-bindings),
[Wii Remotes](SETTINGS.md#wii-remotes)). To quit, close the window (Alt+F4,
Cmd+Q on a Mac): the game shuts down like a Wii does.

## Updates and saves

- The launcher tells you when a new release is out, shows what changed and
  updates the folder in place when you agree. Your settings and saves stay
  ([updates](SETTINGS.md#updates)). On a Mac the disk image is only needed for
  the first install.
- Saves are kept in the program folder. The launcher can move them elsewhere or
  import your save from Dolphin ([saves](SETTINGS.md#saves)).

## Something wrong?

See [troubleshooting](TROUBLESHOOTING.md). Each run writes
`logs/recharged.log` beside the program; attach it to an
[issue](https://github.com/previouslevelgames/recharged/issues) if you need help.
