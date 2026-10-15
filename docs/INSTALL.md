# Install and play

Ready-made packages for Windows, Linux, Steam Deck and macOS. You do not need
to build anything.

> **The packages contain no game files.** You need your own copy of
> **Mario Strikers Charged, USA version, game ID `R4QE01`, revision 1**, as a
> `.iso` or `.rvz` disc image. Other regions and revisions do not work, and
> renaming a file does not change that.

## 1. Download

Open the [latest release](https://github.com/yannicksuter/mscharged-port/releases/latest)
and download the package for your system (`SHA256SUMS` lists the checksums):

| System | Package |
| --- | --- |
| Windows 10/11, 64-bit | `mscharged-<version>-windows-x86_64.zip` |
| Linux, x86_64 | `mscharged-<version>-linux-x86_64.tar.gz` |
| Steam Deck, SteamOS 3.7+ | `mscharged-<version>-steamdeck-x86_64.tar.gz` ([Steam Deck guide](STEAM_DECK.md)) |
| macOS, Apple Silicon | `mscharged-<version>-macos-arm64.tar.gz` |
| macOS 15+, Intel | `mscharged-<version>-macos-x86_64.tar.gz` |

## 2. Unpack

Unpack it into a folder you can write to and keep all files together. Settings,
saves and logs are stored beside the program, so the folder can be anywhere.

## 3. Start

Run **`mscharged.exe`** (Windows) or **`mscharged`** (Linux, macOS). The builds
are not signed, so the system asks once:

- **Windows:** if SmartScreen warns, choose *More info → Run anyway*.
- **macOS:** allow the app under *System Settings → Privacy & Security*, or run
  `xattr -dr com.apple.quarantine <unpacked folder>`. Gamepads also need the
  **Input Monitoring** permission ([details](BUILDING_MACOS.md#controllers)).
- **Linux:** a current distribution (built on Ubuntu 26.04) and a Vulkan driver
  (Mesa or your GPU vendor's) are needed. If the file will not start, mark it
  executable (`chmod +x mscharged`).

## 4. Choose your disc and play

In the launcher open the **Game** page, choose your `.iso` or `.rvz`, and press
**Play**. The disc is read directly; do not extract or mount it, and keep the
file where it is. The first start shows *Preparing graphics* while shaders
compile; later starts reuse them.

On the **Controls** page assign keyboard & mouse, gamepads or Wii Remotes to
players 1-4 ([defaults](FEATURES.md#controls), [Wii Remotes](RUNTIME.md#wii-remote)).

## Updates, saves, problems

- The launcher announces new releases and updates the folder in place
  ([updates](RUNTIME.md#updates)). Test builds of open changes are available
  via *Test* on the Play page ([GitHub builds](BUILDING_GITHUB.md)).
- Saves live in the program folder; the launcher can move them or import a
  Dolphin save ([saves](RUNTIME.md#saves)).
- Each run writes `logs/mscharged.log` beside the program. Attach it to an
  [issue](https://github.com/yannicksuter/mscharged-port/issues) if something
  goes wrong; it can contain your user name in paths.
