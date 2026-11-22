# Replays

Recharged keeps every offline match you play as a replay. You can watch a
match again from the launcher, as fast as four times the normal speed, or
send the file to a friend who has the same game.

## What is recorded

- **Recorded:** Domination matches (exhibition matches), Training and the
  matches of the Road to the Striker Cup.
- **Not recorded:** online and LAN matches, the Striker Challenges,
  Strikers 101 and the demo matches of the title screen.

A replay is written when its match ends, or when you close the game during
the match. A match left within its first second is not kept. Replays go to the
`replays` folder beside the program:

```
replays/replay_20261122-172740.msc   (the date and time the match ended)
```

A replay is small, about 5 KB per minute of play. It holds only what the
match started from (the teams, stadium, rules, cheats, gameplay tweaks and
the match's random seed) and the inputs of every human player. The CPU
players need no inputs: the game makes the same decisions again from the
same start. A replay holds no names and no account or save data.

## Watching a replay

In the launcher, go to **Extras → Replays**. The card lists your replays,
newest first, each on one line with its captains and score, the date, the
match's length and whether it was Training; click one to watch it. **Open
folder** opens the `replays` folder.

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
are now: Fast terrain, Mirror match, Training's options and so on. What only
changes how the game looks or how fast you get through the start stays yours:
Skip intro, Fast menus, and whether the HUD shows (the HUD key, H unless you
changed it, still switches it).

## Keeping replays

```ini
[replays]
record = true   # false: no new replays are kept
keep = 100      # the newest 100 stay, older ones are deleted; 0 keeps all
```

**Keep replays** in **Extras → Replays** is `record`. Only files whose names
begin with `replay_` count for `keep` and are ever deleted: to keep a replay
for good, rename it (for example `final-6-0.msc`) or move it elsewhere.
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
| header | The Recharged version that recorded it (one length byte, then the text), the game ID (6 characters, such as `R4QE01`), the revision (1 byte), the time it was made (8 bytes, seconds since 1970), the size of the match data and of its compressed form (4 bytes each), then a summary for the launcher's list: the match's steps (4 bytes), the home and away captains and the stadium (2 bytes each), the two scores (2 bytes each) and a flags byte (bit 0: Training) |
| rest | The match data, compressed with zlib |

A reader skips header bytes it does not know, so later versions can add to
the header. The match data begin with the setup, then one record per game
step: one byte when no input changed, otherwise only the bytes that changed,
and every 25 steps the checksum and seed the replay is compared with.
