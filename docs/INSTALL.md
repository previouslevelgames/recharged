# Install and play

Ready-made packages for Windows, Linux, Steam Deck and macOS. You do not need
to build anything. On a Steam Deck, follow the [Steam Deck guide](STEAM_DECK.md)
instead.

> **The packages contain no game files.** You need your own copy of
> **Mario Strikers Charged** as an `.iso` or `.rvz` disc image: the USA
> version (game ID `R4QE01`, revision 1) or the European version (game ID
> `R4QP01`, revision 1 or 2). Other regions do not work yet, and renaming a
> file does not change that.

## 1. Download

Open the [latest release](https://github.com/previouslevelgames/recharged/releases/latest)
and download the package for your system:

| System | Package |
| --- | --- |
| Windows 10/11, 64-bit | `recharged-<version>-windows-x86_64.zip` |
| Linux, x86_64 | `recharged-<version>-linux-x86_64.tar.gz` |
| Steam Deck, SteamOS 3.7+ | `recharged-<version>-steamdeck-x86_64.tar.gz` |
| macOS, Apple Silicon | `recharged-<version>-macos-arm64.tar.gz` |
| macOS 15+, Intel | `recharged-<version>-macos-x86_64.tar.gz` |

`SHA256SUMS` on the release page lists the checksums.

**Requirements:** a graphics driver with Vulkan (Windows, Linux), DirectX 12
(Windows) or Metal (macOS). The Linux package is built on Ubuntu 26.04 and
needs a distribution of that age. On an older one, such as Ubuntu 24.04, use
the `steamdeck-x86_64` package: it is the same Linux build, made with older
system libraries.

## 2. Unpack

Unpack it into a folder of its own that you can write to, for example
`Games/recharged` in your home folder. Keep all files together: the program
(`Recharged.app` on macOS), its two modules and the `assets` folder. Settings, saves and logs are stored
in this folder too, so it can be anywhere but should stay in one place.

**Coming from an older version?** It called the program `mscharged`. Unpack
the new package over the old folder and start `recharged`: it takes over your
settings and saves. Then delete the old `mscharged` program and point any
shortcut (on the Steam Deck: *Target*) at `recharged`.

## 3. Start

Run **`recharged.exe`** (Windows), **`Recharged.app`** (macOS) or
**`recharged`** (Linux). The builds are not signed, so the system asks once:

- **Windows:** if SmartScreen warns, choose *More info → Run anyway*.
- **macOS:** before the first start, run
  `xattr -dr com.apple.quarantine <unpacked folder>` in Terminal, or allow the
  app under *System Settings → Privacy & Security* (*Open Anyway*). If macOS
  then starts it from a temporary copy, Recharged offers to do the `xattr`
  step itself and start again. Gamepads and Wii Remotes also need the
  **Input Monitoring** permission
  ([details](TROUBLESHOOTING.md#macos-controllers-do-nothing)).
  The `recharged` file beside the app starts it from Terminal.
- **Linux:** if the file will not start, mark it executable
  (`chmod +x recharged`).

## 4. Choose your game and play

The launcher opens. On the **Game** page add the folder with your `.iso` or
`.rvz` images (**Add folder...**) and choose **Use this image** on the one to
play, or browse for the file itself. A game folder inside the program folder
(`game/`) is found by itself at the first start. The image is read directly; do
not extract or mount it, and keep it where it is. Then press **Play**.

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
  ([updates](SETTINGS.md#updates)).
- Saves are kept in the program folder. The launcher can move them elsewhere or
  import your save from Dolphin ([saves](SETTINGS.md#saves)).

## Something wrong?

See [troubleshooting](TROUBLESHOOTING.md). Each run writes
`logs/recharged.log` beside the program; attach it to an
[issue](https://github.com/previouslevelgames/recharged/issues) if you need help.
