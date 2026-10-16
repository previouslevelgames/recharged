# Steam Deck

The port runs natively on SteamOS: no Proton and no compiling. You install it
once in Desktop Mode, add it to Steam, and from then on start it from your
library in Gaming Mode.

**Quick version** for people who have added non-Steam games before:

1. Download `mscharged-<version>-steamdeck-x86_64.tar.gz` from the
   [latest release](https://github.com/yannicksuter/mscharged-port/releases/latest)
   and extract it to `~/Games/mscharged`.
2. *Games → Add a Non-Steam Game to My Library*, add `mscharged`, leave
   *Compatibility* off.
3. Controller layout: the **Gamepad** template.
4. Start it, choose your disc image on the launcher's **Game** page, set player
   1 to `gamepad1` on **Controls**, press **Play**.

The steps in detail:

## 1. What you need

- SteamOS 3.7 or newer. Check for updates in Gaming Mode under
  *Settings → System*.
- Your own **Mario Strikers Charged disc image for the USA, game ID `R4QE01`,
  revision 1**, as `.iso` or `.rvz`.
  A European copy works from version 1.0.10 on; renaming a file changes
  nothing.
- Optional: a USB or Bluetooth mouse and keyboard make the setup easier.

## 2. Switch to Desktop Mode

Press the STEAM button, then *Power → Switch to Desktop*. In Desktop Mode the
right trackpad moves the mouse, R2 left-clicks, L2 right-clicks and
STEAM + X shows the on-screen keyboard.

## 3. Download and extract

In the Deck's browser open the
[latest release](https://github.com/yannicksuter/mscharged-port/releases/latest)
and download the file ending in **`steamdeck-x86_64.tar.gz`**. The Deck needs
this package, not the `linux-x86_64` one: it is made for SteamOS's system
libraries.

Open the file manager (Dolphin), create the folder `Games/mscharged` in your
Home folder, i.e. `/home/deck/Games/mscharged`. Open the downloaded `.tar.gz`
and *Extract* it there. Keep all files together, including `assets`. If the
archive adds one more folder level, use the folder that contains the
`mscharged` program from now on.

## 4. Put your disc image in place

Copy your `.iso` or `.rvz` to the Deck, for example from a USB drive, into a
`game` folder: `/home/deck/Games/mscharged/game/Mario Strikers Charged.iso`.
Spaces in the name are fine. Do not extract or mount it: the port reads the
image directly, every time you play, so it has to stay where it is.

If your disc image is already on the microSD card, you can choose it there
instead; there is no need for a second copy.

## 5. Add the game to Steam

Still in Desktop Mode, open Steam and choose *Games → Add a Non-Steam Game to
My Library → Browse*. Select the **`mscharged`** file (the program, without an
extension; not the disc image and not the archive) and choose
*Add Selected Programs*.

Then open the new entry's *Properties*:

- Rename it, for example to *Mario Strikers Charged*.
- *Target* must be the `mscharged` program and *Start In* its folder.
- Leave *Launch Options* empty.
- Leave *Compatibility* **unchecked**: this is a native Linux program, Proton
  is not needed.

If Steam later returns straight to the Play button, the file is not marked as
a program: in Dolphin right-click `mscharged`, *Properties → Permissions*, and
enable *Is executable*.

## 6. Artwork (optional)

Non-Steam games start with a blank library tile. To give it artwork, search
for *Mario Strikers Charged* on [SteamGridDB](https://www.steamgriddb.com) and
download a grid image, hero and logo. In Steam's library (Desktop Mode),
right-click the game → *Manage → Set custom artwork* for the grid image; on
the game's page, right-click the empty banner for *Set Custom Background*
and the title for *Set Custom Logo*. The
[Decky Loader](https://decky.xyz) plugin *SteamGridDB* does the same from
Gaming Mode.

## 7. Choose the controller layout

Double-click *Return to Gaming Mode* on the desktop. Open *Library →
Non-Steam*, select the game, open the controller icon and apply the
**Gamepad** template. Steam then presents the Deck's sticks and buttons to the
game as a standard gamepad.

To click in the launcher with the trackpad as well, choose *Edit Layout →
Trackpads*: right trackpad *As Mouse*, click *Left Mouse Click*, and keep the
rest of the Gamepad template.

## 8. First start: disc and controls

Start the game from your library. The launcher appears first.

1. On the **Game** page, browse to your `.iso` or `.rvz`. The launcher checks
   it and shows *R4QE01 - USA* and *Revision 1* when it is the right disc.
2. On the **Controls** page set player 1 to **`gamepad1`** (the first listed
   gamepad) and leave players 2-4 off for one player. Keep the *Default*
   gamepad profile at first.
3. Press **Play**. The first start shows *Preparing graphics* while the
   shaders are compiled; let it finish. Later starts reuse them.

In Gaming Mode the game runs fullscreen. Use the right stick as the Wii
pointer and A to select. Two settings work together here: Steam's layout makes
the Deck a gamepad, and the launcher's player 1 setting hands that gamepad to
the player.

The default mapping (Xbox-style, as on the Deck):

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
the mapping, create a named gamepad profile on the **Controls** page.

From now on, start the game from your library and press **Play**.

## Updates, saves and removal

- **Updates:** when a new release is out, the launcher shows its notes and
  asks; it then updates the folder in place and restarts. Your settings and
  saves stay.
- **Saves** are in `/home/deck/Games/mscharged/original-main-credits-data/nand`.
  Copy that folder to back them up.
- **Removal:** in Steam choose *Manage → Remove non-Steam game from your
  library*, then delete the folder (back up the saves first).

## Troubleshooting

| Problem | Check |
| --- | --- |
| Buttons do nothing | Steam layout is *Gamepad* and the launcher's player 1 is `gamepad1` |
| No gamepad in the launcher | Quit, start the game through Steam in Gaming Mode, reopen *Controls* |
| Cannot click in the launcher | Hold STEAM and use the right trackpad as a mouse; STEAM + R2 clicks |
| Disc is rejected | It must be the USA version, R4QE01, revision 1 (Europe from version 1.0.10) |
| Steam returns straight to Play | *Is executable*, *Target* and *Start In* (step 5) |
| Stalls or crashes | Attach `logs/mscharged.log` from the game folder to an [issue](https://github.com/yannicksuter/mscharged-port/issues) |

More in the general [troubleshooting](TROUBLESHOOTING.md) page.
