# Replays

Recharged keeps the matches you play as replays, offline and online. You can
watch a match again from the launcher, as fast as four times the normal speed,
look through its stats (goals, shots, items, each player's numbers), or send
the file to a friend who has the same game.

## What is recorded

- **Recorded:** Domination matches (exhibition matches), Training, the
  matches of the Road to the Striker Cup, and online and LAN matches: each
  game of a set is a replay of its own, with every player's inputs as the
  match confirmed them.
- **Not recorded:** the Striker Challenges, Strikers 101 and the demo matches
  of the title screen.

A replay is written when its match ends, or when you close the game during
the match. A match left within its first second is not kept. Replays go to the
`replays` folder beside the program:

```
replays/replay_20261122-172740.msc   (the date and time the match ended)
```

A replay is small, about 5 KB per minute of play. It holds what the match
started from (the teams, stadium, rules, cheats, gameplay tweaks and the
match's random seed), the inputs of every human player and the match's stats.
The CPU players need no inputs: the game makes the same decisions again from
the same start. A replay holds no account or save data; an online match's
replay names its two players (their profile names in a set, else their Miis'
names), an offline one names nobody.

## Watching a replay

In the launcher, go to **Replays**. The page lists your replays,
newest first, each on one line with its captains and score, the date, the
match's length and whether it was Training; click one to watch it. The chart
button at the end of a line shows the match's stats. **Open folder** opens the
`replays` folder.

You can also:

- drop a `.msc` file on the launcher's window;
- open a `.msc` file with Recharged (choose the Recharged program in your
  system's *Open with*, or drop the file on the program);
- start it from a terminal: `recharged --replay FILE` or just
  `recharged FILE.msc`.

If you have unsaved changes in the launcher, it asks about them first, as
**Play** does.

The game goes straight into the match, without the menus. While it plays:

| Key | Gamepad | What it does |
| --- | --- | --- |
| **Right** | D-pad right | Faster: twice, then four times the normal speed |
| **Left** | D-pad left | Slower again; at normal speed it pauses the replay |

The speed shows in the corner for a few seconds as the match begins and after
each change; while the replay is paused it stays, and **Right** goes on. When the replay ends, *Replay finished* appears and the game
closes a few seconds later; close the window to stop sooner. A replay started
from a file does not open the launcher again.

A replay never reads or writes your save. A Cup match plays back as a
single match, so watching it does not change your Cup.

A match watched live (the online server's **Watch** link) plays a moment
behind the players. If it is more than a few seconds behind, for example when
you join a match that is already under way, it catches up as fast as your
computer can: the sound is off, cutscenes that can be skipped are skipped,
and the corner shows how far behind it still is and about when it is live.

Cutscenes play as the players saw them: a part of one a player skipped
(a goal's celebration, its replay) is skipped in the replay too, at the same
moment.

## Match stats

The chart button of a replay's line opens its stats on the Replays page
(**Replays**, Escape or B go back to the list; **Watch** plays it):

- **The match:** who played (online, their names; offline, the controllers
  or the CPU), the captains and sidekicks, the score, the date, the stadium,
  how long it was played and by which rules.
- **Match summary:** both sides' numbers side by side: goals, shots, shots on
  goal, possession, passes, interceptions, hits, steals, dekes, items,
  Skillshots, Mega Strikes and fouls.
- **Goals:** each goal in order, with its time, scorer, assist, kind
  (one-timer, Skillshot, Mega Strike, own goal) and the score after it.
- **Timeline:** the goals, items, hits, steals and fouls of both sides along
  the match; point at one to see what it was.
- **Shots:** where each shot was taken, on the field to scale; a filled dot
  scored. Point at a dot for its time, kind and shooter.
- **Items:** each side's items, the most used first.
- **Players:** each player's goals, assists, shots, passes, hits, steals and
  items.

The numbers are the game's own: its stats keeper counts them while the match
is played, and Recharged keeps them in the replay. Replays from before match
stats have none (their chart button stays grey). The captains' faces and the
sidekicks' pictures come from your game image; without one, initials stand in.

### The game image a replay needs

A replay plays only with the release and revision of the game it was
recorded with, for example *USA, revision 1* or *Europe, revision 2*: the
game's data differ between them. The launcher uses your chosen game image when
it fits, otherwise a fitting image from your game folder
([game images](SETTINGS.md#game-images)), without changing your choice. When
none fits, it says which release the replay needs.

From a terminal, `recharged --replay FILE` (or `recharged FILE.msc`) goes
through the launcher the same way. With `--disc IMAGE` as well, the game
starts at once with that image, without the launcher; for a replay of another
release it shows a message naming the release the replay needs.

### Settings during a replay

The match plays with the gameplay tweaks it was recorded with, whatever yours
are now: Fast terrain, Mirror match, Training's options and so on. An online
match plays by the rules it had online: the gameplay tweaks that online play
switches off stay off, and a set plays by its rule set's tweaks. What only
changes how the game looks or how fast you get through the start stays yours:
Skip intro, Fast menus, and whether the HUD shows (the HUD key, H unless you
changed it, still switches it).

## Keeping replays

```ini
[replays]
record = true   # false: no new replays are kept
keep = 100      # the newest 100 stay, older ones are deleted; 0 keeps all
```

**Keep replays** in **Replays** is `record`. It decides only what
stays on this PC: online games go to the server either way, which streams them
to spectators while they are played and keeps the ones it asks for. Only files
whose names begin with `replay_` count for `keep` and are ever deleted: to keep
a replay for good, rename it (for example `final-6-0.msc`) or move it elsewhere.
Renamed replays still show in the launcher and play the same way.

## Sharing replays

Send the `.msc` file. Your friend needs the same release and revision of the
game (the launcher's Play page shows yours, for example *Europe, revision 2*)
and, for the replay to play exactly as it happened, the same version of
Recharged.

## When a replay plays differently

The game is deterministic: the same start and the same inputs give the same
match. A replay recorded by another version of Recharged can still differ,
for example when a fix changed how something plays. Twice a second, the game
compares its own state with the recording. When they no longer agree,
the game says *This replay no longer matches its recording.* and goes on; from
there the players' recorded inputs no longer fit what happens on the field.

The log (`logs/recharged.log`) has a line for each replay:

```
Replay: ended after 7176 of 7176 steps; 288 of 288 checks matched the recording
```

A replay that cannot be read (a damaged or cut file) does not start the game;
the launcher says why.

## Command line

| Option | What it does |
| --- | --- |
| `--replay FILE` | Plays the replay `FILE` |
| `FILE.msc` | The same, for a `.msc` file given alone |
| `--disc IMAGE` | Plays it with this game image, without the launcher |

All the program's options are in [settings](SETTINGS.md#command-line-options).

## How it works

The original game contains an input recorder its developers used, which no
menu of the retail game reaches. When a match loads, the game gives it a
fresh random seed and hands the recorder the match's setup. Then, for each of
the 50 game steps per second, it records the inputs it applies for every
human player, with the game's checksum and frame. To play a recording back,
the game reads those inputs instead of the controllers, and it can run extra
steps per frame to play faster.

Recharged turns this recorder on and keeps what it writes in the replay file
(the game's [tweak series](ARCHITECTURE.md) holds the hooks). It also stores
what the game's own setup leaves out: all gameplay settings and cheats, the
mode, the gameplay tweaks, Training's options and the CPU difficulty the
match loaded with. To start a replay, the game passes the title screen, sets
the recorded match up without the save and loads it; the original loading
then reads the recorded seed and setup.

### The file

A `.msc` file is little-endian:

| Bytes | What |
| --- | --- |
| 8 | `MSCRPLY` and the byte 0x1A |
| 2 | File version (1) |
| 2 | Size of the header that follows |
| header | The Recharged version that recorded it (one length byte, then the text), the game ID (6 characters, such as `R4QE01`), the revision (1 byte), the time it was made (8 bytes, seconds since 1970), the size of the match data and of its compressed form (4 bytes each), then a summary for the launcher's list: the match's steps (4 bytes), the home and away captains and the stadium (2 bytes each), the two scores (2 bytes each) and a flags byte (bit 0: Training, bit 1: an online or LAN match, bit 2: the match's stats are in the match data); in replays with stats or names, then each side's player name (its kind, one length byte, the UTF-8 text) |
| rest | The match data, compressed with zlib |

A reader skips header bytes it does not know, so later versions can add to
the header. The match data begin with the setup (which also lists the parts of
cutscenes the players skipped, and when, and ends with the match's stats),
then one record per game step: one byte when no input changed, otherwise only
the bytes that changed, and every 25 steps the checksum and seed the replay is
compared with.
