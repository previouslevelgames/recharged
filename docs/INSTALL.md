# Install and play

Ready-made packages for Windows, Linux, Steam Deck and macOS. You do not need
to build anything. On a Steam Deck, follow the [Steam Deck guide](STEAM_DECK.md)
instead.

> **The packages contain no game files.** You need your own copy of
> **Mario Strikers Charged for the USA, game ID `R4QE01`, revision 1**, as an
> `.iso` or `.rvz` disc image.
> The European version follows with version 1.0.10; other regions do not work
> yet, and renaming a file does not change that.

## 1. Download

Open the [latest release](https://github.com/yannicksuter/mscharged-port/releases/latest)
and download the package for your system:

| System | Package |
| --- | --- |
| Windows 10/11, 64-bit | `mscharged-<version>-windows-x86_64.zip` |
| Linux, x86_64 | `mscharged-<version>-linux-x86_64.tar.gz` |
| Steam Deck, SteamOS 3.7+ | `mscharged-<version>-steamdeck-x86_64.tar.gz` |
| macOS, Apple Silicon | `mscharged-<version>-macos-arm64.tar.gz` |
| macOS 15+, Intel | `mscharged-<version>-macos-x86_64.tar.gz` |

`SHA256SUMS` on the release page lists the checksums.

**Requirements:** a graphics driver with Vulkan (Windows, Linux), DirectX 12
(Windows) or Metal (macOS). The Linux package is built on Ubuntu 26.04 and
needs a distribution of that age. On an older one, such as Ubuntu 24.04, use
the `steamdeck-x86_64` package: it is the same Linux build, made with older
system libraries.

## 2. Unpack

Unpack it into a folder of its own that you can write to, for example
`Games/mscharged` in your home folder. Keep all files together: the program,
its two modules and the `assets` folder. Settings, saves and logs are stored
in this folder too, so it can be anywhere but should stay in one place.

## 3. Start

Run **`mscharged.exe`** (Windows) or **`mscharged`** (Linux, macOS). The
builds are not signed, so the system asks once:

- **Windows:** if SmartScreen warns, choose *More info → Run anyway*.
- **macOS:** allow the app under *System Settings → Privacy & Security*, or
  run `xattr -dr com.apple.quarantine <unpacked folder>` in Terminal. Gamepads
  and Wii Remotes also need the **Input Monitoring** permission
  ([details](TROUBLESHOOTING.md#macos-controllers-do-nothing)).
- **Linux:** if the file will not start, mark it executable
  (`chmod +x mscharged`).

## 4. Choose your disc and play

The launcher opens. On the **Game** page choose your `.iso` or `.rvz`. The
disc is read directly; do not extract or mount it, and keep the file where it
is. Then press **Play**.

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
`logs/mscharged.log` beside the program; attach it to an
[issue](https://github.com/yannicksuter/mscharged-port/issues) if you need help.
