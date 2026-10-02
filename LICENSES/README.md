# License scope and third-party notices

## Original project work

The original port code, tools, and documentation contributed to `mscharged-port`
are dedicated to the public domain under [CC0 1.0 Universal](../LICENSE), to the
extent their authors hold the relevant rights. The root `LICENSE` contains the
dedication text; the [Creative Commons legal code](https://creativecommons.org/publicdomain/zero/1.0/legalcode.en)
is the official reference.

Third-party source, reconstructed game code, game assets, and trademarks are
not relicensed by this dedication. The project grants no rights on behalf of
their owners.

## Source dependencies

The [dependency inventory](../extern/README.md) records the 21 initial submodules,
their purposes, and their exact revisions. License texts remain in those
upstream checkouts. The bootstrap compiles selected decomp utilities and nod,
including its Cargo dependencies; Corrosion supplies the CMake/Rust integration.
The launcher also builds SDL3 and Dear ImGui and uses nod for disc checks.
The optional Aurora core check also builds Aurora, Abseil, fmt, xxHash, and Tracy
from prepared submodule sources. The separate Linux GX diagnostic also builds
Dawn and its selected nested sources, zlib-ng, libpng, FreeType, Zstandard,
SQLite, and ImGui's WebGPU backend. It uses Dawn's nested Abseil as the shared
provider. SDL's included third-party codecs retain the notices in their source
files. Unused optional libraries remain available for future integration.

The following summaries identify principal notices, not a replacement for
file-specific terms or notices on nested third-party material:

| Source | Licensing reference at the selected checkout |
| --- | --- |
| Charged decompilation | [CC0 dedication](../extern/mscharged-decomp/LICENSE) for original contributions; [separate third-party notices](../extern/mscharged-decomp/LICENSES). Reconstructed game code is not relicensed by this port. |
| Aurora | [MIT](../extern/aurora/LICENSE), copyright Luke Street. |
| Dawn | [BSD-style terms](../extern/dawn/LICENSE); nested sources retain their own notices. |
| SDL | [zlib license](../extern/sdl/LICENSE.txt). |
| Abseil | [Apache 2.0](../extern/abseil-cpp/LICENSE). |
| xxHash | [BSD terms](../extern/xxhash/LICENSE). |
| fmt | [MIT terms and exception](../extern/fmt/LICENSE). |
| zlib-ng | [zlib terms](../extern/zlib-ng/LICENSE.md). |
| libpng | [PNG notices](../extern/libpng/LICENSE). |
| FreeType | [License selection](../extern/freetype/LICENSE.TXT): FreeType License or GPL; contributed portions have additional notices. |
| Dear ImGui | [MIT](../extern/imgui/LICENSE.txt). |
| RmlUi | [MIT](../extern/rmlui/LICENSE.txt). |
| SQLite | [Public-domain scope and exceptions](../extern/sqlite/LICENSE.md). |
| Zstandard | [BSD option](../extern/zstd/LICENSE) or [GPL option](../extern/zstd/COPYING). |
| nod | [MIT](../extern/nod/LICENSE-MIT) or [Apache 2.0](../extern/nod/LICENSE-APACHE); Rust dependencies have their own terms. |
| Corrosion | [MIT](../extern/corrosion/LICENSE). |
| GoogleTest | [BSD terms](../extern/googletest/LICENSE). |
| Tracy | [BSD terms](../extern/tracy/LICENSE). |
| Qt Base | [Module license texts](../extern/qtbase/LICENSES); Core/GUI/Widgets provide LGPL 3.0 and other alternatives, with file-specific and third-party notices. |
| Qt Tools | [Tool license texts](../extern/qttools/LICENSES); tools such as `lrelease` use GPL 3.0 with the Qt GPL exception or commercial terms. Do not label every tool LGPL. |
| Qt Translations | [Module license texts](../extern/qttranslations/LICENSES) and [upstream license rules](../extern/qttranslations/licenseRule.json). |

Checking out an optional library does not establish that it will be distributed
with the game. Select and document the relevant license option and distribution
materials when integrating each component.

The current graphics integration selects the FreeType License in
[FTL.TXT](../extern/freetype/docs/FTL.TXT) and Zstandard's BSD option.
Portions of this software are copyright © 2026 The FreeType Project
(https://freetype.org). All rights reserved. FreeType's contributed components
retain their own notices. Dawn's nested shader tools, Vulkan headers/utilities,
Abseil, and build-time Jinja/MarkupSafe retain the license texts at their recorded
checkouts; see the [selected source inventory](../extern/README.md#dawns-selected-linux-graphics-sources).
No release package has been prepared; packaging must collect the actual linked
components' notices and any required accompanying material.

## Adding dependencies and preparing releases

For each dependency, record its upstream source, purpose, pinned revision, and
applicable license notices. Preserve notices in submodules and copied material,
and identify any local patches. Include dependencies brought in by upstream
build systems in this review.

When distributing a build, include the license texts and notices required for
the material actually distributed. A link to a submodule alone does not replace
required notices in a release package. Include corresponding source or relinking
materials where a dependency's terms require them.

## Launcher artwork

The selected `assets/launcher/header.png` comes from
[SteamGridDB hero 8926, uploaded by Jiquita](https://www.steamgriddb.com/hero/8926).
It is third-party artwork outside the CC0 dedication. The source metadata does
not specify a redistribution license. See [artwork provenance](../assets/launcher/README.md).

The launcher's Roboto Medium font comes from the pinned Dear ImGui font bundle
and uses Apache 2.0, as recorded in its [font inventory](../extern/imgui/docs/FONTS.md).
The original font, embedded notices, and full Apache 2.0 text are copied into
the build's `assets/launcher/` directory. It is outside the CC0 dedication.

Disc images and extracted game data are not distributed with this project.
Players supply their own game data.
