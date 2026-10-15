# Steam Deck

Use the `steamdeck-x86_64` package (SteamOS 3.7 or newer; check for updates in
Gaming Mode under *Settings → System*). The Deck build has not been tested on
an actual Deck yet, so this is the documented setup, not a guaranteed one. You
do not need to compile anything.

**1. Desktop Mode.** Press the STEAM button, then *Power → Switch to Desktop*.
In Desktop Mode the right trackpad moves the mouse, R2 left-clicks, L2
right-clicks and STEAM + X shows the on-screen keyboard. A USB or Bluetooth
mouse and keyboard make setup easier but are optional.

**2. Download.** In the Deck's browser open the
[latest release](https://github.com/yannicksuter/mscharged-port/releases/latest)
and download the file ending in `steamdeck-x86_64.tar.gz`.

**3. Extract to a permanent folder.** In Dolphin (the file manager), create
`Games/mscharged` in your Home folder (`/home/deck/Games/mscharged`), open the
downloaded `.tar.gz` and *Extract* it there. Keep all files together,
including `assets`. If extraction adds one more folder, use the folder that
holds the `mscharged` program below.

**4. Copy your game disc.** The port reads your **USA disc, game ID R4QE01,
revision 1**, as `.iso` or `.rvz` directly: do not extract or mount it. A
European copy does not work, and renaming a file changes nothing. Copy it from
a USB drive (or another route) into a `game` folder inside
`/home/deck/Games/mscharged`, for example
`/home/deck/Games/mscharged/game/Mario Strikers Charged.iso`. Spaces in the
name are fine. The file has to stay there, because the port reads it every
time you play. No game files are included in the download.

**5. Add the program to Steam.** In Steam (Desktop Mode) choose *Games → Add a
Non-Steam Game to My Library → Browse*, select the `mscharged` file (no
extension) and *Add Selected Programs*. Add the program, not the disc image or
the archive. Then open the entry's *Properties*: rename it if you like, check
that *Target* is the `mscharged` program and *Start In* its folder, leave
*Launch Options* empty, and leave *Compatibility* unchecked (it is a native
Linux program). If Steam cannot start it, right-click `mscharged` in Dolphin,
*Properties → Permissions*, and enable *Is executable*.

**6. Controller layout.** Double-click *Return to Gaming Mode*, open *Library →
Non-Steam*, select the game, open the controller icon and apply the
**Gamepad** template. This makes Steam present the Deck's sticks and buttons as
a standard gamepad. To click in the launcher with the trackpad, you can set
*Edit Layout → Trackpads*: right trackpad *As Mouse*, click *Left Mouse Click*,
and keep the rest of the Gamepad template.

**7. Choose the disc.** Start the game from Steam. The launcher appears; on the
**Game** page browse to your `.iso` or `.rvz`. The disc is not added to Steam
separately.

**8. Assign the controls.** On the **Controls** page set player 1 to
`gamepad1` (or the first listed gamepad), leave players 2-4 off for one player
and keep the *Default* gamepad profile at first. Two settings are involved:
Steam's layout makes the Deck a gamepad, and the port's player 1 setting gives
that gamepad to the player. The default (Xbox-style) mapping:

| Deck | Wii input |
| --- | --- |
| Left stick | Nunchuk stick (move) |
| Right stick | Wii pointer |
| A / B | Wii A (confirm) / B (back) |
| X / Y | Wii 1 / 2 |
| D-pad | Wii D-pad |
| ☰ menu / View | Wii + / − |
| L1 / L2 | Nunchuk C / Z |
| R2 | Shake Wii Remote (hit an opponent) |
| R1 | Shake Nunchuk (switch items) |

Nothing has to be shaken physically: the shake actions are buttons. To change
them, create a named gamepad profile on the Controls page.

**9. Play.** Press **Play** in the launcher. The first start shows *Preparing
graphics* while shaders compile; let it finish, later starts reuse the cache.
In Gaming Mode the game opens fullscreen (`--window` in the launch options
keeps a window); use the right stick as the pointer and A to select. From now
on start it from your Steam Library and press Play.

If something does not work:

| Problem | Check |
| --- | --- |
| Buttons do nothing | Steam layout is Gamepad and launcher player 1 is `gamepad1` |
| No gamepad in the launcher | Quit, start through Steam in Gaming Mode, reopen Controls |
| Cannot click in the launcher | Hold STEAM and use the right trackpad as a mouse; STEAM + R2 clicks |
| Disc is rejected | It must be USA R4QE01, revision 1 |
| Steam returns straight to Play | *Is executable*, *Target* and *Start In* |
| Stalls or crashes after setup | Possibly a port issue; attach `logs/mscharged.log` to an [issue](https://github.com/yannicksuter/mscharged-port/issues) |
