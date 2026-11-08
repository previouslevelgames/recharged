# Contributing

Thanks for playing and for helping Recharged get better. Most problems have a
quick answer in [troubleshooting](docs/TROUBLESHOOTING.md); please check it
first. Problems of the online server itself belong in
[its repository](https://github.com/previouslevelgames/recharged-wfc-srv/issues).

## Reporting a problem

Open an [issue](https://github.com/previouslevelgames/recharged/issues/new/choose)
with:

- **The version**: the launcher's **About** page shows it, as does the first
  line of the log.
- **Your system**: Windows, Linux (which distribution), Steam Deck or macOS,
  and your graphics card.
- **Your controllers**: keyboard & mouse, which gamepads, Wii Remotes (over
  Bluetooth or a DolphinBar).
- **What happened** and what you did just before: the menu, mode, stadium or
  captains if it was in a match.
- **The log**: `logs/recharged.log` in the program folder. It is replaced at
  the next start, so copy it right after the problem. On Windows a crash also
  leaves `recharged-crash.dmp` there; attach it too. The log can contain your
  user name in file paths.

A screenshot (key **P**, saved in `screenshots/`) helps for anything visual.

**Short stutters** the first time an effect appears are usually a graphics
pipeline missing from the package's list. The log names it in a line starting
with `Graphics: new pipeline compiled while drawing`; such logs let us add it to
the next release.

**Behavior that differs from the Wii**: Recharged runs the original game code,
including its original bugs, unless a listed
[tweak or fix](docs/FEATURES.md#optional-tweaks) changes it. Describe what the
Wii does, ideally with a video or a way to reproduce it, and say whether any
tweaks were on.

Never attach disc images, extracted game files or other people's saves. The
log, screenshots and your own description are all we need.

## Ideas

Ideas are welcome as issues: say what they would change for players. Before
you start on a larger change, open an issue first so we can agree on the
design. Help with interface design and artwork is especially welcome.

## Pull requests

We review every pull request with care and expect the same care in return.

- Open it against `main`; `stable` only takes release pull requests.
- One change per pull request, with what it does, why, and how you tested it.
- It builds, and `ctest --preset debug` passes ([building](docs/BUILDING.md)).
- Changes to the game's code are patches, never edits of the decompilation;
  every intended change to how the game plays belongs in
  `mscharged-decomp-tweaks/` ([patch rules](patches/README.md)).
- A change players can notice has been tried in the game, not only in tests.
- Commits follow the repository's style, with the issue number, e.g.
  `fix(input): ... (#46)`.

### AI tools

Using AI tools is fine; we use them too (see the
[disclaimer](README.md#disclaimer-ai-assistance)). What counts is that you own
the result: you have read and understood every line, you can explain it, and
you have tested it.

We reject pull requests built on carefree vibe coding: code that was obviously
not read or checked, changes nobody tested, unrelated edits, invented game
behavior, or descriptions that do not match the code. We close them without a
detailed review. If the idea behind one is good, we may implement it ourselves.
