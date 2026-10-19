# Features

What Recharged, the native PC port of Mario Strikers Charged, offers. The game
itself is the original: its modes, rules, physics, timing and presentation
come from the reconstructed source, unchanged.

## The game

- **Every offline mode** of the original: Domination (exhibition matches),
  the Road to the Striker Cup with its Fire, Crystal and Striker cups,
  Strikers 101 lessons and challenges, the Hall of Fame, cheats, options and
  credits.
- **Up to four players** on one machine, in any mix of keyboard & mouse,
  gamepads and Wii Remotes.
- **Saves** like on the Wii: progress, unlocks, trophies and options are
  stored in Recharged's own data folder.
- **The USA and European releases.** The European version, *Mario Strikers
  Charged Football*, runs in its 60 Hz mode and keeps its own saves; its first
  revision plays with the code of the second.
- **Languages** of the disc: English, French and Spanish with the USA release;
  English, German, French, Spanish and Italian with the European one.
- **Movies, music and sound** as on the console.
- **The logo reads Mario Strikers RECHARGED** on the title screen, in the main
  menu and on the captain select, with every release: the game's own animated
  letters, with an R and an E in front of CHARGED
  ([#63](https://github.com/previouslevelgames/recharged/issues/63)).

## Controls

- **Keyboard & mouse** play as a Wii Remote with Nunchuk; the mouse is the
  pointer. Every key can be changed in the launcher, up to two per action,
  and the middle and side mouse buttons (Mouse 4, Mouse 5) can take a key's
  place ([#62](https://github.com/previouslevelgames/recharged/issues/62)).
- **Gamepads**: Xbox, PlayStation, Switch Pro, GameCube (USB adapter) and
  other standard controllers stand in for the Wii Remote with Nunchuk the game
  is played with, with rumble (its strength can be set). The right stick
  moves the pointer, or the gyro of a controller that has one (PlayStation,
  Switch Pro): turn it to point, like a Wii Remote
  ([#14](https://github.com/previouslevelgames/recharged/issues/14)).
  **Gamepad profiles** say which controller button presses each Wii button: a
  built-in Default that gives each controller family its own layout
  (Nintendo's A is its right button) plus up to 16 named profiles (up to two
  buttons per Wii button, or none; sticks can be swapped, and a stick pushed
  one way can press a button, such as the right stick as the D-pad for
  dekes). A profile also sets the sticks'
  inner and outer dead zones and the triggers' press point, tried live on the
  controller in the launcher; the defaults play like a Nunchuk on the Wii
  ([#62](https://github.com/previouslevelgames/recharged/issues/62)). A
  profile belongs to one kind of controller (Xbox and PlayStation share
  profiles), so each gamepad is offered the profiles that fit it; they are
  kept for the next session.
- **Real Wii Remotes with Nunchuk**, paired over **Bluetooth** or through a
  **Mayflash DolphinBar** (mode 4): pointer, rumble, the Remote speaker and
  battery level, with the sensor bar above or below the screen and the Wii's
  sensitivity and speaker volume settings
  ([#60](https://github.com/previouslevelgames/recharged/issues/60)).
- **Mouse beside a controller**: a single player on a Wii Remote or gamepad
  can also point and press A/B with the mouse.

### Default bindings

| Wii input | Keyboard / mouse | Gamepad (Xbox names) |
| --- | --- | --- |
| Pointer | Mouse | Right stick or gyro |
| A / B | Enter or left click / Esc or right click | A / B |
| D-pad | Arrow keys | D-pad |
| Nunchuk stick | W A S D | Left stick |
| Nunchuk C / Z | C / V | LB / LT |
| 1 / 2 | Z / X | X / Y |
| + / − / HOME | Tab / - / Home | Start / Back / Guide |
| Shake Remote / Nunchuk | E / Q | RT / RB |

**Hotkeys** work whoever plays: **P** saves a screenshot, **H** shows or hides
the HUD in a match and **Alt+Enter** (Option+Return on a Mac) switches between
fullscreen and a window. P and H can be changed under Controls → Hotkeys.

**No HUD** (Controls → Hotkeys → *Show the HUD when a match starts* off):
matches start without the scoreboard (score, clock, items, ability meter),
the player markers and the captions (replay label, goal banner, super ability
card), for clean recordings and screenshots; the HUD key shows them. The Mega
Strike meter and the goalie's Mega Strike defence stay, as you need them to
play.

## Picture and sound

- Vulkan on Linux, Vulkan or DirectX 12 on Windows (Advanced → Graphics API),
  Metal on macOS; on PCs with two GPUs, a choice of graphics card (Advanced →
  Graphics card).
- Window or fullscreen on any monitor, freely sized, 4:3 or 16:9; Alt+Enter
  switches while playing.
- **3D at your window's resolution** (default) instead of the Wii's 640 x 448,
  or fixed 2x-4x; menus and the HUD keep their original artwork.
- Picture styles: **sharp** (default) or clean fill the window with the game's
  picture; soft keeps the console's TV look, with the TV signal's empty border.
- **Antialiasing**: 4x MSAA by default, or FXAA, 4x MSAA + FXAA, or 4x SSAA,
  which also smooths the edges inside textures (Display → Antialiasing).
- **Performance mode** (the gear on Play): lighter matches without
  antialiasing, crowd, background NPCs, ambient effects, bloom or stadium
  shadows. Gameplay, hazards and their effects remain unchanged.
- **High frame rates**: the console's 60 FPS by default, or 120, 144, your
  display's refresh rate or unlimited (Display → Frame rate). The game keeps its
  own speed: matches still simulate at their fixed 50 Hz and every frame is
  drawn from the game's own blend of its last two steps, so the extra frames
  are real motion, not interpolation added by Recharged.
- V-sync and a performance overlay (FPS, CPU and GPU load) in any corner.
- Master volume, mute and **sound levels** (Audio): music, sound effects,
  voices, the crowd, menus and HUD, cutscenes and movies each from 0 to 100 %,
  on top of the game's own music, effect and voice options.
- **Screenshots** with the P key, or the one chosen under Controls → Hotkeys.

## Optional tweaks

Off by default; with all of them off the game plays as on the Wii. Four fixes
of the original game are always on: a goal net no longer stays dented after a
goal ([#19](https://github.com/previouslevelgames/recharged/issues/19)),
electrocuted players flash with the game's lightning texture instead of a
debug texture ([#33](https://github.com/previouslevelgames/recharged/issues/33)),
the NK ("No Kritter") bug is gone: a Boo deke into his own goalie, a
teleport into a goal or a Chain Chomp hit during a deke no longer lets later
shots pass through the goalies
([#38](https://github.com/previouslevelgames/recharged/issues/38)),
and no star with "x3" flashes in the top corners when the HUD leaves the
screen after a goal
([#68](https://github.com/previouslevelgames/recharged/issues/68)).
The launcher's **Gameplay** page (or `[gameplay]` in `recharged.ini`) turns them on:

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
- **Fair ball contests**: when players of both teams reach a loose ball in
  the same instant, the one the game updates first takes it. The game
  shuffles that order at every kickoff in a way that puts the home team
  first slightly more often (52 % of the time on average, up to 55 %). With
  this option the order is shuffled fairly, and again every simulation step.
  On by default, as it is recommended to every player; online matches always
  play with it.
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
- **Robot goalies**: both goalies wear the robot Kritter skin of the Super
  Team, the hidden robot team of Super Mario Strikers on the GameCube. The
  game's disc still holds its texture, unused and made for the older game's
  goalie; Recharged lays it onto Charged's goalie (assets/gameplay). Only the
  look changes.
- **Super Team** (work in progress): the captain and sidekicks of a team (the away team, the home
  team or both) become the Super Team's robots. Their model comes from your own
  Super Mario Strikers (GameCube) disc image: the launcher finds one in your
  game folder, or asks for it when you turn the option on. Each robot is fitted
  to its character's size and keeps the character's animations and play;
  Bowser, Petey Piranha and Boo keep their own look.

`--no-tweaks` turns them off for one run.

## Launcher

A start window that checks your game image, explains what is missing, and keeps
every setting in one place: players and controllers, keyboard keys, gamepad
profiles, display, audio, gameplay tweaks and advanced options. It asks before
playing with unsaved changes. Everything is also stored in `recharged.ini`
beside the program ([settings](SETTINGS.md)).

Its **game images** table lists the Mario Strikers Charged images in your game
folder, with their region, revision, game ID, format and size and the small
banner the game uses for its save, read from your own image. Click the one to
play; images of versions this build cannot run yet are listed with the reason
([game images](SETTINGS.md#game-images)).

It also **updates Recharged**: when a newer release is out it shows the release
notes and asks, then installs it in place and restarts; only the program's own
files are replaced. **Release / Test** on the Play page switches between
official releases and test builds of changes not yet released, and back,
asking before it installs anything; with several changes in testing, Test
lets you choose which one to follow ([updates](SETTINGS.md#updates)).

## Texture packs

Texture packs made for Dolphin replace the game's textures, from a folder or
straight from their `.zip` file, with several packs in an order you choose
(Gameplay → Mods, [settings](SETTINGS.md#texture-packs)).

## Discord status

While the Discord app runs on the same computer, your Discord profile shows the
menu you are in or the match you play, with its mode, stadium, captains, score
and match time ([settings](SETTINGS.md#discord-status)).

## Not supported (yet)

- **The Japanese and Korean versions.** Their decompilation is still being
  completed.
- **Online play.** Nintendo Wi-Fi Connection has shut down, so for now
  Recharged behaves like a Wii without internet. The netcode is being reworked
  to make online play functional again, and options for matchmaking and
  lobbies are being explored.
- **Mods.** A modding framework is in the works; until then, the optional
  tweaks above change the game and texture packs change its look.
