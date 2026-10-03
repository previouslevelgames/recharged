# Source dependencies

All entries below are Git submodules. The gitlink in the parent repository is
the authoritative full commit pin; the linked commits identify the selected
selection. No submodule is configured to advance to an upstream branch during
ordinary builds.

Shallow clones are preferred to keep initial downloads manageable. Fetch more
history explicitly when an upstream comparison or patch rebase needs it.

The bootstrap compiles selected Charged utilities and nod's disc reader using
Corrosion. The graphical launcher builds SDL3 and Dear ImGui, uses nod for disc
checks, and copies the Roboto Medium font from ImGui's font bundle. The remaining
sources are checked out for runtime, graphics, UI, and test integration. The
optional Aurora core check additionally builds Aurora, Abseil, fmt, xxHash,
and Tracy. The experimental startup also enables Aurora DVD through the same
prepared nod target and real MEM2 allocation through the Aurora patch series.
It builds the patched original game allocator and memory initialization.
The separate Linux graphics diagnostic builds Aurora GX/Dawn and prepared image,
font, cache, and ImGui providers without enabling original game startup.
The Linux `scene` preset shares those providers with the launcher and combines
GX and DVD/NL for a bounded preview of actual static Wii assets; original
scene/task initialization remains pending.
See [runtime development](../docs/RUNTIME.md).
Optional components may be removed after their role is
settled. Adding a source submodule does not automatically make an upstream
FetchContent declaration use it.

| Source | Selected baseline | Commit | Purpose | Local notices |
| --- | --- | --- | --- | --- |
| [mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp) | Development snapshot; matching allocator and events | [`4a9ab8151d9a`](https://github.com/yannicksuter/mscharged-decomp/commit/4a9ab8151d9a74e321e52b36c95d1e157d41af60) | Utilities, original startup/NL files and selected rendering; full entry compiles for scanning, event integration and complete native game pending | [LICENSE](mscharged-decomp/LICENSE) |
| [aurora](https://github.com/encounter/aurora) | Development snapshot | [`d664382f5700`](https://github.com/encounter/aurora/commit/d664382f57002fbff143911c81b06b7e7a4b3ab4) | Host checks, Wii DVD/MEM2, GX diagnostics and static asset preview; original scene/task integration pending | [LICENSE](aurora/LICENSE) |
| [dawn](https://github.com/encounter/dawn) | Aurora-selected commit | [`1155e0ed5311`](https://github.com/encounter/dawn/commit/1155e0ed531126f33a1279afa029349651ca1c93) | GX/Vulkan diagnostic and static preview backend | [LICENSE](dawn/LICENSE) |
| [sdl](https://github.com/libsdl-org/SDL) | 3.4.10 | [`8e37db5e797b`](https://github.com/libsdl-org/SDL/commit/8e37db5e797b6167f3a00d697d816a684bd259c7) | Launcher window, rendering, dialogs, and controller input | [LICENSE.txt](sdl/LICENSE.txt) |
| [abseil-cpp](https://github.com/abseil/abseil-cpp) | 20240722.0 | [`4447c7562e3b`](https://github.com/abseil/abseil-cpp/commit/4447c7562e3bc702ade25105912dce503f0c4010) | Aurora standalone utility fallback | [LICENSE](abseil-cpp/LICENSE) |
| [xxhash](https://github.com/Cyan4973/xxHash) | 0.8.3 | [`e626a72bc232`](https://github.com/Cyan4973/xxHash/commit/e626a72bc2321cd320e953a0ccf1584cad60f363) | Aurora hashing | [LICENSE](xxhash/LICENSE) |
| [fmt](https://github.com/fmtlib/fmt) | 12.1.0 | [`407c905e45ad`](https://github.com/fmtlib/fmt/commit/407c905e45ad75fc29bf0f9bb7c5c2fd3475976f) | Aurora text formatting | [LICENSE](fmt/LICENSE) |
| [zlib-ng](https://github.com/zlib-ng/zlib-ng) | 2.3.3 | [`12731092979c`](https://github.com/zlib-ng/zlib-ng/commit/12731092979c6d07f42da27da673a9f6c7b13586) | Aurora compression provider | [LICENSE.md](zlib-ng/LICENSE.md) |
| [libpng](https://github.com/pnggroup/libpng) | 1.6.58 | [`3061454d980d`](https://github.com/pnggroup/libpng/commit/3061454d980de7d53608f594194cfac722721d2a) | PNG image support | [LICENSE](libpng/LICENSE) |
| [freetype](https://github.com/freetype/freetype) | 2.14.3 | [`0a0221a1347e`](https://github.com/freetype/freetype/commit/0a0221a1347e2f1e07c395263540026e9a0aa7c7) | UI font rasterization | [LICENSE.TXT](freetype/LICENSE.TXT) |
| [imgui](https://github.com/ocornut/imgui) | 1.91.9b docking | [`4806a1924ff6`](https://github.com/ocornut/imgui/commit/4806a1924ff6181180bf5e4b8b79ab4394118875) | Launcher interface; bundled Roboto font has separate Apache 2.0 terms | [LICENSE.txt](imgui/LICENSE.txt) |
| [rmlui](https://github.com/encounter/RmlUi) | Aurora-selected commit | [`f00a0fee3839`](https://github.com/encounter/RmlUi/commit/f00a0fee38391b2f927114e11bea18dc0a7dba1e) | Optional HTML/CSS-style in-game UI | [LICENSE.txt](rmlui/LICENSE.txt) |
| [sqlite](https://github.com/sqlite/sqlite) | 3.51.3 | [`a5333afb9ad1`](https://github.com/sqlite/sqlite/commit/a5333afb9ad1aa473f8963b92caeaa955f47dc74) | Aurora shader/cache database | [LICENSE.md](sqlite/LICENSE.md) |
| [zstd](https://github.com/facebook/zstd) | 1.5.7 | [`f8745da6ff1a`](https://github.com/facebook/zstd/commit/f8745da6ff1ad1e7bab384bd1f9d742439278e99) | Cache and data compression | [LICENSE](zstd/LICENSE) |
| [nod](https://github.com/encounter/nod) | 2.0.0-alpha.12 | [`ebd80cac99b4`](https://github.com/encounter/nod/commit/ebd80cac99b48a323200d84365e07a58cc27412d) | ISO/RVZ access in bootstrap and Aurora DVD | [LICENSE-MIT](nod/LICENSE-MIT) |
| [corrosion](https://github.com/corrosion-rs/corrosion) | 0.6.1 | [`1499b14e4906`](https://github.com/corrosion-rs/corrosion/commit/1499b14e4906a2890f5cee1547c8848db261753d) | CMake/Rust integration for nod | [LICENSE](corrosion/LICENSE) |
| [googletest](https://github.com/google/googletest) | 1.17.0 | [`52eb8108c5bd`](https://github.com/google/googletest/commit/52eb8108c5bdec04579160ae17225d66034bd723) | Aurora test framework | [LICENSE](googletest/LICENSE) |
| [qtbase](https://github.com/qt/qtbase) | 6.8.4-lts-lgpl | [`ed77a3ca9ef1`](https://github.com/qt/qtbase/commit/ed77a3ca9ef1bf5e33b6f32ea41110734fb14e88) | Optional desktop settings UI: Core, GUI, Widgets | [LICENSES](qtbase/LICENSES) |
| [qttools](https://github.com/qt/qttools) | 6.8.4-lts-lgpl | [`fd835497a2c0`](https://github.com/qt/qttools/commit/fd835497a2c0d441ea08a35b09b6abd2b5c29b48) | Optional Qt translation and development tools | [LICENSES](qttools/LICENSES) |
| [qttranslations](https://github.com/qt/qttranslations) | 6.8.4-lts-lgpl | [`ba2badad1dcb`](https://github.com/qt/qttranslations/commit/ba2badad1dcb34f0755bc79a27da0485991c5a77) | Optional Qt UI translations | [LICENSES](qttranslations/LICENSES) |
| [tracy](https://github.com/wolfpld/tracy) | 0.14.1 | [`30997d5ca6bb`](https://github.com/wolfpld/tracy/commit/30997d5ca6bb632cc10807a1da8a6d3de0aeeb3c) | Runtime profiling support | [LICENSE](tracy/LICENSE) |

## Selection and integration notes

- The decomp pin is a published development commit. It is incomplete and not
  fully linked. No 1.0 or complete-decomp release is assumed to exist.
  The current snapshot marks `Game/main.cpp` as matching for the original Wii
  build; it still requires native host/service/resource integration for this port.
- Aurora dependency versions follow the declarations in its pinned
  `extern/CMakeLists.txt`, `cmake/AuroraDependencyVersions.cmake`, and test setup.
  Corrosion follows nod's pinned CMake requirement. Qt components use the same
  publicly available 6.8.4 source-release family.
- The Aurora core check supplies prepared SDL, Abseil, fmt, xxHash, and Tracy
  targets before configuring Aurora and disables extra FetchContent downloads.
  GX, DVD, CARD, THP, and RmlUi are disabled for that check.
  The `startup` preset enables DVD but keeps the other components disabled.
  The separate `graphics` preset enables GX and Vulkan; `scene` combines GX/DVD
  with the launcher and selected original core/NL services. The core check remains
  in the GX-disabled presets. Graphics supplies pinned image/font/cache/ImGui targets;
  other graphics backends and upstream tests are disabled.
  SDL's pinned build expects legacy macro escaping; `cmake/SDL.cmake` scopes
  policy `CMP0219` to `OLD` on CMake 4.4+ without changing its checkout.
- Dawn and RmlUi use the compatibility forks selected by Aurora, under
  `encounter/dawn` and `encounter/RmlUi`. Their original projects are
  [Dawn](https://dawn.googlesource.com/dawn) and
  [RmlUi](https://github.com/mikke89/RmlUi). Preserve their upstream history and
  notices; changes made for this port belong in our patch series.
- SQLite's upstream Git source is converted to `sqlite3.c`/`sqlite3.h` by
  `tools/prepare_sqlite.py`, using host GNU Make, Tcl, and a C compiler.
  Generated source/tool provenance and contents are verified before building.
- FreeType uses its prepared Git source and requires the recorded `subprojects/dlg`
  checkout for export. Optional Brotli, HarfBuzz, PNG, zlib, and bzip2 providers
  are disabled for this font build.
- Aurora already contains a THP decoder and its card implementation. A separate
  FFmpeg or kabufuda checkout is not required by the selected Aurora graph.
- The current launcher uses Dear ImGui's SDL3 and SDLRenderer3 backends. SDL's
  built-in PNG reader loads the header image. Qt and RmlUi remain optional
  sources for later UI work; neither is built by the current targets.

## Dependencies owned by these projects

Dawn has its own recorded submodules and DEPS graph, including shader toolchains,
platform headers, and development tools. Keep those revisions associated with
Dawn. A full recursive checkout can be large and is not needed for the bootstrap.
Qt Tools also records nested HTML-viewer sources. Initialize required nested
submodules at their recorded commits before preparing the corresponding source.

nod records Rust dependencies in `Cargo.lock`; its prepared CMake source is
patched to pass `LOCKED` to Corrosion for metadata and compilation. The build
uses prepared local nod/Corrosion sources and Cargo's pinned compression source
crates. It may download these crates on the first build, with no lockfile update.
Sharing compression providers with Aurora remains runtime integration work.
Aurora can find system libraries or fetch/prebuild its own copies. Runtime
builds here select prepared local providers and disable extra source downloads.
The graphics preset shares Dawn's nested Abseil provider with Aurora; the
host/startup presets retain the direct Abseil submodule. GoogleTest is unused
because upstream test targets are disabled.
The current targets do not fetch additional Git repositories while building.

### Dawn's selected Linux graphics sources

`cmake/Dawn.cmake` selects these direct nested gitlinks, recursively at their
recorded pins. The preparation manifest includes both the selection and commits.
The remaining Dawn browser, platform, toolchain, and test sources are omitted.

| Path under `dawn/` | Recorded commit | Purpose |
| --- | --- | --- |
| `third_party/abseil-cpp` | `df548c50b2cda67158364d3d23c63043881b391d` | Single shared Abseil provider |
| `third_party/jinja2` | `c3027d884967773057bf74b957e3fea87e5df4d7` | Build-time generator templates |
| `third_party/markupsafe` | `4256084ae14175d38a3ff7d739dca83ae49ccec6` | Build-time Jinja dependency |
| `third_party/spirv-headers/src` | `942fe4b988359a0750b79f0ae7ed735994d3147d` | SPIR-V declarations/grammars |
| `third_party/spirv-tools/src` | `5b5a25231a3543559b4f2cb27fe725a08b76b5d0` | Shader validation/transforms |
| `third_party/vulkan-headers/src` | `f9973cd97e6f3584707e7ef1c425e336f1b92a5b` | Vulkan API declarations |
| `third_party/vulkan-utility-libraries/src` | `6673ca41d73d36c3c838451e66dd30dd1ec46a97` | Vulkan utility headers |

See [graphics setup](../docs/RUNTIME.md#independent-gxvulkan-diagnostic) for
initialization and build commands. The host Vulkan loader/driver, desktop
libraries, compiler, Python, Make, and Tcl remain platform prerequisites.

To fetch the top-level sources at their recorded pins:

```sh
git -c submodule.recurse=false submodule update --init --checkout
```

Initialize nested dependencies only for the components being developed, for
example:

```sh
git -C extern/qttools submodule update --init --recursive --checkout
```

The preparation tool rejects missing required nested checkouts instead of
silently exporting an incomplete dependency. See [build instructions](../docs/BUILDING.md)
and [patch development](../patches/README.md).

All third-party terms remain in force. See [license scope](../LICENSES/README.md)
for the distinction between original port work and dependency source.
