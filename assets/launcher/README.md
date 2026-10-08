# Application resources

## Header

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

## Application icon

`icon.png` is the original, unmodified 512 x 512 Mario portrait.

- Source: [SteamGridDB icon 103028](https://www.steamgriddb.com/icon/103028).
- Uploaded by [roryypc](https://www.steamgriddb.com/profile/76561198982504400).
- Image: [original PNG](https://cdn2.steamgriddb.com/icon/7c867647488e862e745b6992a0f882e4.png).
- Downloaded: 2026-10-08.
- SHA-256: `b9a96660da3bfae23581b628705a6096e48aeda6887632551505ba980bd6d3a6`.

The PNG is embedded for the launcher and game window; SDL also uses it for the
running application's macOS Dock icon. Window icon support on Linux depends on
the desktop/compositor. `icon.ico` contains 16, 24, 32, 48, 64, 128 and 256 pixel
versions for the Windows executable, generated with:

```sh
magick icon.png -define icon:auto-resize=256,128,64,48,32,24,16 icon.ico
```

The icon artwork and its format conversion are outside the project's CC0
dedication. The source metadata does not specify a redistribution license;
rights remain with the respective owners.

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
