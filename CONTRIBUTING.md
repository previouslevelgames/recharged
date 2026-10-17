# Reporting problems

Thanks for playing and for taking the time to report what went wrong. Most
problems have a quick answer in [troubleshooting](docs/TROUBLESHOOTING.md);
please check it first.

## Bug reports

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

## Short stutters

A stutter the first time an effect appears is usually a graphics pipeline
that was missing from the package's list. The log names it in a line starting
with `Graphics: new pipeline compiled while drawing`. Attaching such logs lets
us add those pipelines to the next release, so the stutter disappears for
everyone.

## Behavior that differs from the Wii

Recharged runs the original game code, including its original bugs, unless a
listed [tweak or fix](docs/FEATURES.md#optional-tweaks) changes it. If
something plays differently from the Wii, describe what the Wii does, ideally
with a video or a way to reproduce it, and say whether any tweaks were on.

## Game files

Never attach disc images, extracted game files or saves of other people to an
issue. Only the log, screenshots and your own descriptions are needed.
