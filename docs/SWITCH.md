# Nintendo Switch (homebrew)

> [!IMPORTANT]
> **The Switch version of Recharged is homebrew.** It is not an official
> Nintendo Switch game: it is not on the eShop, there is no installable
> version (`.nsp`) and an unmodified Switch cannot start it. It runs only on a
> console with custom firmware (Atmosphère) and the Homebrew Menu, which
> starts `recharged.nro` from the SD card. Setting up custom firmware is not
> covered here.

The Switch build is in development and has no release yet. These steps are
for its test builds.

## What you need

- **A Switch with custom firmware:** Atmosphère with the Homebrew Menu.
- **Your own disc image** of Mario Strikers Charged as `.rvz`, `.iso` or
  `.wbfs`: the USA version (game ID `R4QE01`, revision 1) or the European
  version, *Mario Strikers Charged Football* (`R4QP01`, revision 1 or 2). No
  game files are included.
- **Room on the SD card** for the image: about 1.8 GB as `.rvz`, 2.1 GB as
  `.wbfs` and 4.7 GB as `.iso`.

## Install

1. Download `recharged-<version>-switch-aarch64.zip` from the newest Switch
   test build: a pre-release named `test-pr101-…` on the
   [releases page](https://github.com/previouslevelgames/recharged/releases).
2. Unpack it to the root of the SD card. This creates `/switch/recharged/`
   with `recharged.nro`.
3. Copy the disc image to `/switch/recharged/game/`. A FAT32 card holds no
   file over 4 GB: an `.rvz` or `.wbfs` fits, an `.iso` needs an exFAT card.
   A `.wbfs` split into `.wbfs` and `.wbf1` (as Wii Backup Manager makes them)
   works too. Recharged plays the first supported image in that folder; to
   pick another one, set `[game] disc` in `recharged.ini`.

To update, unpack a newer package the same way and replace the files. Your
settings, saves and logs stay.

## Start

Hold **R** while starting any game from the HOME Menu, then pick **Recharged**
in the Homebrew Menu. This is title mode, which gives homebrew the console's
full memory. Started from the Album, homebrew gets only a few hundred MB, and
Recharged stops with the message *Recharged needs the full memory of title
mode*.

The first start prepares the graphics shaders, which takes a while. The
controllers play in the console's player order. To quit, close Recharged from
the HOME Menu like a game.

## Settings and saves

All in `/switch/recharged/`:

- **Settings:** copy `recharged.ini.example` to `recharged.ini` and edit it
  on a PC. Without it, Recharged uses the default settings. The keys are in
  [Settings](SETTINGS.md).
- **Saves:** in `data/`.
- **Log:** `logs/recharged.log`. After a crash, attach it and Atmosphère's
  report from `/atmosphere/crash_reports/` to the bug report.
