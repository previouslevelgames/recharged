# Launcher resources

`header.png` is the original, unmodified 1920 x 620 PNG selected for the launcher
header. Preserve its aspect ratio when displaying it.

- Source: [SteamGridDB hero 8926](https://www.steamgriddb.com/hero/8926).
- Uploaded by [Jiquita](https://www.steamgriddb.com/profile/76561198074965726).
- Image: [original PNG](https://cdn2.steamgriddb.com/hero/82b04cd5aa016d979fe048f3ddf0e8d3.png).
- Downloaded: 2026-10-01.
- SHA-256: `d1451de5493ffecd9a50541456a823e5e464edb6cae39e6a99e615afdae92617`.

This third-party artwork is outside the project's CC0 dedication. The source
metadata does not specify a redistribution license; rights remain with the
respective owners.

CMake copies the image into `assets/launcher/header.png` beside the `mscharged`
executable. The launcher preserves its proportions and crops vertically to fit
the available header space. The source file is unchanged.

## Font

The launcher also copies `misc/fonts/Roboto-Medium.ttf` from the pinned
[Dear ImGui source](../../extern/imgui/misc/fonts/Roboto-Medium.ttf).
The upstream [font inventory](../../extern/imgui/docs/FONTS.md) identifies it
as Roboto Medium under the Apache License 2.0. Its embedded notice reads:
Copyright 2011 Google Inc. All Rights Reserved. The font is copied unchanged.
The full Apache 2.0 text is copied as `LICENSE-APACHE`
alongside the font, image, and this notice. That generic license text is stored
in [LICENSES/Apache-2.0.txt](../../LICENSES/Apache-2.0.txt), copied verbatim from
the pinned Abseil source's Apache 2.0 text.

Resources are located relative to the executable, independently of the current
working directory. Keep `assets/launcher/` with a copied executable.
