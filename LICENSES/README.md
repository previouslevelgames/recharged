# License scope and third-party notices

## Original project work

Recharged's original code, tools and documentation (this repository) are
dedicated to the public domain under [CC0 1.0 Universal](../LICENSE), to the
extent their authors hold the relevant rights. The root `LICENSE` contains the
dedication text; the [Creative Commons legal code](https://creativecommons.org/publicdomain/zero/1.0/legalcode.en)
is the official reference. Material submitted for inclusion in the project is
dedicated under the same terms, to the extent the submitter holds the rights.

This dedication does not cover third-party source, the reconstructed game code,
game assets or trademarks, and the project grants no rights on behalf of their
owners. Mario Strikers Charged was developed by Next Level Games and published
by Nintendo; Recharged is an unofficial project, unaffiliated with either
company.

## Game data

No disc images or game files are part of this repository or its packages.
Players supply their own copy of the game.

## What the packages contain

Each release package contains the program and game module, built from this
repository, the pinned decompilation and the dependencies below, plus the
notices for everything it includes:

| In the package | Contents |
| --- | --- |
| `LICENSE`, `LICENSES/README.md` | This project's dedication and this overview |
| `LICENSES/dependencies/` | The license and notice files of every prepared source dependency, including nested components and build-time tools |
| `LICENSES/rust/` | `Cargo.lock` and the notices of every Rust crate in nod's dependency graph |
| `LICENSES/toolchain/` (Windows) | The LLVM-MinGW C++ runtime (`libc++`, `libunwind`) and mingw-w64 runtime and winpthreads notices |
| `LICENSES/dxc/`, `LICENSES/windows-sdk/` (Windows) | The notices of the bundled Microsoft DirectX shader compilers |
| `assets/launcher/` | Launcher artwork with its provenance, and the Roboto font with the Apache 2.0 text |
| `SOURCE-MANIFEST.json` | The exact source commits, patches and file hashes of the build |

The notice collection is deliberately broad: it does not assert that every
prepared component is linked into the program.

## Principal licenses

The full terms are in each dependency's checkout and in the packages; this
table names the principal ones.

| Source | License |
| --- | --- |
| Charged decompilation | CC0 for original contributions, with separate third-party notices. The reconstructed game code is not relicensed by this project. |
| Aurora | MIT, copyright Luke Street |
| Dawn | BSD-style terms; nested sources keep their own notices |
| SDL | zlib; bundled third-party code keeps the notices in its source files |
| Dear ImGui | MIT |
| nod | MIT or Apache 2.0; its Rust crates have their own terms |
| Corrosion | MIT |
| FreeType | FreeType License (selected from FTL or GPL); contributed components have additional notices |
| libpng | PNG notices |
| zlib-ng | zlib |
| Zstandard | BSD (selected from BSD or GPL) |
| SQLite | Public domain |
| fmt | MIT terms and exception |
| xxHash | BSD |
| Tracy | BSD |
| Abseil | Apache 2.0 |
| LLVM-MinGW runtime (Windows) | Apache 2.0 with LLVM exception (`libc++`, `libunwind`); mingw-w64 runtime and winpthreads notices |
| DirectX shader compilers (Windows) | Microsoft DXC and Windows SDK redistributables; versions and hashes in [windows_graphics.json](../tools/windows_graphics.json) |

Portions of this software are copyright © 2026 The FreeType Project
(https://freetype.org). All rights reserved.

## Application artwork

The launcher header (`assets/launcher/header_back.png` and `header_front.png`) is
made from Nintendo's key art for Mario Strikers Charged, as the
[Super Mario Wiki](https://www.mariowiki.com/File:MSC_Scene.jpg) keeps it.
The application icon (`assets/launcher/icon.png` and its Windows `.ico`
conversion) comes from
[SteamGridDB icon 103028, uploaded by roryypc](https://www.steamgriddb.com/icon/103028).
Both are third-party artwork outside the CC0 dedication; their sources do not
specify a redistribution license. The Recharged logo
(`assets/launcher/logo.png`) is outside the CC0 dedication too. The screenshot
`docs/images/launcher.png` shows the launcher with this header and the logo. See
[artwork provenance](../assets/launcher/README.md).

The launcher's Roboto Medium font comes from the pinned Dear ImGui font bundle
and uses Apache 2.0, as recorded in its
font inventory. The font, its embedded notices
and the full [Apache 2.0 text](Apache-2.0.txt) are copied beside the program
in `assets/launcher/`. It is outside the CC0 dedication.

## Adding a dependency

Record its upstream source, purpose, pinned commit and license in
[`extern/README.md`](../extern/README.md) and here, keep its notices, and make
sure packaging collects them (`tools/package_build.py`). Include dependencies
that upstream build systems bring in. When distributing a build, include the
license texts and notices for the material actually distributed, and
corresponding source or relinking materials where a license requires them.
