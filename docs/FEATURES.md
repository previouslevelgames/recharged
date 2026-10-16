# Features

What the native port of Mario Strikers Charged offers. The game itself
is the original: its modes, rules, physics, timing and presentation come from
the reconstructed source, unchanged.

## The game

- **Every offline mode** of the original: Domination (exhibition matches),
  the Road to the Striker Cup with its Fire, Crystal and Striker cups,
  Strikers 101 lessons and challenges, the Hall of Fame, cheats, options and
  credits.
- **Up to four players** on one machine, in any mix of keyboard & mouse,
  gamepads and Wii Remotes.
- **Saves** like on the Wii: progress, unlocks, trophies and options are
  stored in the port's own data folder.
- **Languages** of the USA release: English, French and Spanish.
- **Movies, music and sound** as on the console.

## Controls

- **Keyboard & mouse** play as a Wii Remote with Nunchuk; the mouse is the
  pointer. Every key can be changed in the launcher (up to two keys per
  action).
- **Gamepads**: Xbox, PlayStation, Switch Pro, GameCube (USB adapter) and
  other standard controllers stand in for the Wii Remote with Nunchuk the game
  is played with, with rumble. The right stick moves the pointer. **Gamepad
  profiles** say which controller button presses each Wii button: a built-in
  Default that gives each controller family its own layout (Nintendo's A is
  its right button) plus up to 16 named profiles (up to two buttons per Wii
  button, or none; sticks can be swapped). A profile belongs to one kind of
  controller (Xbox and PlayStation share profiles), so each gamepad is offered
  the profiles that fit it; they are kept for the next session.
- **Real Wii Remotes with Nunchuk**, paired over **Bluetooth** or through a
  **Mayflash DolphinBar** (mode 4): pointer, rumble and battery level, with the
  sensor bar above or below the screen.
- **Mouse beside a controller**: a single player on a Wii Remote or gamepad
  can also point and press A/B with the mouse.

### Default bindings

| Wii input | Keyboard / mouse | Gamepad (Xbox names) |
| --- | --- | --- |
| Pointer | Mouse | Right stick |
| A / B | Enter or left click / Esc or right click | A / B |
| D-pad | Arrow keys | D-pad |
| Nunchuk stick | W A S D | Left stick |
| Nunchuk C / Z | C / V | LB / LT |
| 1 / 2 | Z / X | X / Y |
| + / − / HOME | Tab / - / Home | Start / Back / Guide |
| Shake Remote / Nunchuk | E / Q | RT / RB |

Press **P** for a screenshot.

## Picture and sound

- Vulkan on Linux, Vulkan or DirectX 12 on Windows (Advanced → Graphics API),
  Metal on macOS.
- Window or fullscreen on any monitor, freely sized, 4:3 or 16:9.
- **3D at your window's resolution** (default) instead of the Wii's 640 x 448,
  or fixed 2x-4x; menus and the HUD keep their original artwork.
- Picture styles: **sharp** (default) or clean fill the window with the game's
  picture; soft keeps the console's TV look, with the TV signal's empty border.
- **4x antialiasing**, on by default (Display → Antialiasing).
- **Performance mode** (the gear on Play): lighter matches without 4x
  antialiasing, crowd, background NPCs, ambient effects, bloom or stadium
  shadows. Gameplay, hazards and their effects remain unchanged.
- **High frame rates**: the console's 60 FPS by default, or 120, 144, your
  display's refresh rate or unlimited (Display → Frame rate). The game keeps its
  own speed: matches still simulate at their fixed 50 Hz and every frame is
  drawn from the game's own blend of its last two steps, so the extra frames
  are real motion, not interpolation added by the port.
- V-sync, a performance overlay (FPS, CPU and GPU load) in any corner, master
  volume and mute; music, effects and voice levels stay in the game's own
  options.
- **Screenshots** with the P key.

## Optional tweaks

Off by default; with all of them off the game plays as on the Wii. Three fixes
of the original game are always on: a goal net no longer stays dented after a
goal ([#19](https://github.com/yannicksuter/mscharged-port/issues/19)),
electrocuted players flash with the game's lightning texture instead of a
debug texture ([#33](https://github.com/yannicksuter/mscharged-port/issues/33)),
and the NK ("No Kritter") bug is gone: a Boo deke into his own goalie, a
teleport into a goal or a Chain Chomp hit during a deke no longer lets later
shots pass through the goalies
([#38](https://github.com/yannicksuter/mscharged-port/issues/38)).
The launcher's **Gameplay** page (or `[gameplay]` in `mscharged.ini`) turns them on:

- **Skip intro**: start at the main menu, without the notice screens, studio
  logo, intro movie and title screen.
- **Fast menus**: skip menu transitions, also in the pause menu and on the
  screens after a match (matches, movies and credits unchanged).
- **No Mega Strikes** for both teams, the usual competitive rule. Skillshots
  stay, and Strikers 101 lessons keep their own rules.
- **Unlock everything**: all characters, stadiums and cheats, through the
  game's own unlock switch; the save is not changed.
- **CPU vs CPU**: in Domination, press Done on Choose Sides without picking a
  side and the CPU plays both teams. Choose Sides in the pause menu can also
  hand a match over to the CPU.
- **Fast terrain**: every stadium plays like the fast, dry field of Bowser
  Stadium: the same running speed, ball roll and bounce, no sliding on ice or
  mud and no slow sand in the Sand Tomb. Stadium looks and hazards stay;
  online matches are unchanged.
- **Thunder Island walls**: Thunder Island's open sides get the electric
  fence of every other stadium, invisible but with its sparks: the ball and
  items bounce off it, and players knocked into it are electrocuted instead
  of falling off the island. Online matches are unchanged.
- **Mirror match**: both teams can pick the same captain in Domination. The
  away team wears an away kit, in the menus as on the field: the game's own,
  or for Mario, Luigi, Wario and Waluigi, who have none, one made from their
  own kit (Mario blue, Luigi sky blue, Wario blue, Waluigi orange). Peach
  wears her blue kit; its menu pictures and Mega Strike hand are made the
  same way. Online matches are unchanged.
- **Blue Peach**: Peach's team wears her blue and yellow kit on the field
  (Peach, her sidekicks and her goalie). The game has it on the disc but only
  uses it against a captain of her colours, and nobody else is pink. Her menu
  pictures, Mega Strike hand, stadium banners and crowd have no blue version
  and stay pink.

`--no-tweaks` turns them off for one run.

## Launcher

A start window that checks your disc, explains what is missing, and keeps every
setting in one place: players and controllers, keyboard keys, gamepad
profiles, display, audio, gameplay tweaks and advanced options. It asks before
playing with unsaved changes. Everything is also stored in `mscharged.ini`
beside the program ([settings](SETTINGS.md)).

It also **updates the port**: when a newer release is out it shows the release
notes and asks, then installs it in place and restarts; only the program's own
files are replaced. **Release / Test** on the Play page switches between
official releases and test builds of changes not yet released, and back,
asking before it installs anything ([updates](SETTINGS.md#updates)).

## Not supported (yet)

- **Other versions of the game.** For now only the USA release (R4QE01,
  revision 1) runs. The European version (revisions 1 and 2) follows with
  version 1.0.10; for the Japanese and Korean versions the decompilation is
  still being completed.
- **Online play.** Nintendo Wi-Fi Connection has shut down, so for now the
  port behaves like a Wii without internet. The netcode is being reworked to
  make online play functional again, and options for matchmaking and lobbies
  are being explored.
- **Mods.** A modding framework is in the works; until then, the optional
  tweaks above change the game.
