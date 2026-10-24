# Nintendo Switch

> The Switch build is in development: there is no download yet. These steps
> are for its test builds.

**You need:** a Switch running Atmosphère with the Homebrew Menu, and your own
Mario Strikers Charged disc image as `.rvz`, `.iso` or `.wbfs` (the same
versions as on PC, see [Install](INSTALL.md)).

## Install

1. Download `recharged-<version>-switch-aarch64.zip` from the test build.
2. Unpack it to the root of the SD card. This creates `/switch/recharged/`
   with `recharged.nro`.
3. Copy the disc image to `/switch/recharged/game/`. A FAT32 card holds no
   file over 4 GB: an `.rvz` fits (about 1.8 GB), an `.iso` needs an exFAT
   card, and a `.wbfs` must be split into `.wbfs` and `.wbf1` (as Wii Backup
   Manager makes them).

## Start

Hold **R** while starting any game from the HOME Menu, then pick **Recharged**
in the Homebrew Menu. Started from the Album, it does not get enough memory.

The first start prepares the graphics shaders, which takes a while. The
controllers play in the console's player order.

## Settings and saves

All in `/switch/recharged/`: settings in `recharged.ini` (edit it on a PC; the
keys are in [Settings](SETTINGS.md)), saves in `data/`, the log in
`logs/recharged.log`. After a crash, attach that log and Atmosphère's report
from `/atmosphere/crash_reports/` to the bug report.
