# Source dependencies

All entries below are Git submodules. The gitlink in the parent repository is
the authoritative full commit pin; the linked commits identify the initial
selection. No submodule is configured to advance to an upstream branch during
ordinary builds.

Shallow clones are preferred to keep initial downloads manageable. Fetch more
history explicitly when an upstream comparison or patch rebase needs it.

The bootstrap compiles selected Charged utilities and nod's disc reader using
Corrosion. The remaining sources are checked out for runtime, graphics, UI, and
test integration. Optional components may be removed after their role is
settled. Adding a source submodule does not automatically make an upstream
FetchContent declaration use it.

| Source | Selected baseline | Commit | Purpose | Local notices |
| --- | --- | --- | --- | --- |
| [mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp) | Development snapshot | [`de9c1c8a3673`](https://github.com/yannicksuter/mscharged-decomp/commit/de9c1c8a367334ee079a7989f1b3967883d830c9) | Available Wii game source; bootstrap subset only | [LICENSE](mscharged-decomp/LICENSE) |
| [aurora](https://github.com/encounter/aurora) | Development snapshot | [`d664382f5700`](https://github.com/encounter/aurora/commit/d664382f57002fbff143911c81b06b7e7a4b3ab4) | Planned console API compatibility and runtime | [LICENSE](aurora/LICENSE) |
| [dawn](https://github.com/encounter/dawn) | Aurora-selected commit | [`1155e0ed5311`](https://github.com/encounter/dawn/commit/1155e0ed531126f33a1279afa029349651ca1c93) | Planned WebGPU rendering backend | [LICENSE](dawn/LICENSE) |
| [sdl](https://github.com/libsdl-org/SDL) | 3.4.10 | [`8e37db5e797b`](https://github.com/libsdl-org/SDL/commit/8e37db5e797b6167f3a00d697d816a684bd259c7) | Planned window, input, and host platform services | [LICENSE.txt](sdl/LICENSE.txt) |
| [abseil-cpp](https://github.com/abseil/abseil-cpp) | 20240722.0 | [`4447c7562e3b`](https://github.com/abseil/abseil-cpp/commit/4447c7562e3bc702ade25105912dce503f0c4010) | Aurora standalone utility fallback | [LICENSE](abseil-cpp/LICENSE) |
| [xxhash](https://github.com/Cyan4973/xxHash) | 0.8.3 | [`e626a72bc232`](https://github.com/Cyan4973/xxHash/commit/e626a72bc2321cd320e953a0ccf1584cad60f363) | Aurora hashing | [LICENSE](xxhash/LICENSE) |
| [fmt](https://github.com/fmtlib/fmt) | 12.1.0 | [`407c905e45ad`](https://github.com/fmtlib/fmt/commit/407c905e45ad75fc29bf0f9bb7c5c2fd3475976f) | Aurora text formatting | [LICENSE](fmt/LICENSE) |
| [zlib-ng](https://github.com/zlib-ng/zlib-ng) | 2.3.3 | [`12731092979c`](https://github.com/zlib-ng/zlib-ng/commit/12731092979c6d07f42da27da673a9f6c7b13586) | Aurora compression provider | [LICENSE.md](zlib-ng/LICENSE.md) |
| [libpng](https://github.com/pnggroup/libpng) | 1.6.58 | [`3061454d980d`](https://github.com/pnggroup/libpng/commit/3061454d980de7d53608f594194cfac722721d2a) | PNG image support | [LICENSE](libpng/LICENSE) |
| [freetype](https://github.com/freetype/freetype) | 2.14.3 | [`0a0221a1347e`](https://github.com/freetype/freetype/commit/0a0221a1347e2f1e07c395263540026e9a0aa7c7) | UI font rasterization | [LICENSE.TXT](freetype/LICENSE.TXT) |
| [imgui](https://github.com/ocornut/imgui) | 1.91.9b docking | [`4806a1924ff6`](https://github.com/ocornut/imgui/commit/4806a1924ff6181180bf5e4b8b79ab4394118875) | Debug and development UI | [LICENSE.txt](imgui/LICENSE.txt) |
| [rmlui](https://github.com/encounter/RmlUi) | Aurora-selected commit | [`f00a0fee3839`](https://github.com/encounter/RmlUi/commit/f00a0fee38391b2f927114e11bea18dc0a7dba1e) | Optional HTML/CSS-style in-game UI | [LICENSE.txt](rmlui/LICENSE.txt) |
| [sqlite](https://github.com/sqlite/sqlite) | 3.51.3 | [`a5333afb9ad1`](https://github.com/sqlite/sqlite/commit/a5333afb9ad1aa473f8963b92caeaa955f47dc74) | Aurora shader/cache database | [LICENSE.md](sqlite/LICENSE.md) |
| [zstd](https://github.com/facebook/zstd) | 1.5.7 | [`f8745da6ff1a`](https://github.com/facebook/zstd/commit/f8745da6ff1ad1e7bab384bd1f9d742439278e99) | Cache and data compression | [LICENSE](zstd/LICENSE) |
| [nod](https://github.com/encounter/nod) | 2.0.0-alpha.12 | [`ebd80cac99b4`](https://github.com/encounter/nod/commit/ebd80cac99b48a323200d84365e07a58cc27412d) | ISO/RVZ access in the bootstrap; later Aurora disc integration | [LICENSE-MIT](nod/LICENSE-MIT) |
| [corrosion](https://github.com/corrosion-rs/corrosion) | 0.6.1 | [`1499b14e4906`](https://github.com/corrosion-rs/corrosion/commit/1499b14e4906a2890f5cee1547c8848db261753d) | CMake/Rust integration for nod | [LICENSE](corrosion/LICENSE) |
| [googletest](https://github.com/google/googletest) | 1.17.0 | [`52eb8108c5bd`](https://github.com/google/googletest/commit/52eb8108c5bdec04579160ae17225d66034bd723) | Aurora test framework | [LICENSE](googletest/LICENSE) |
| [qtbase](https://github.com/qt/qtbase) | 6.8.4-lts-lgpl | [`ed77a3ca9ef1`](https://github.com/qt/qtbase/commit/ed77a3ca9ef1bf5e33b6f32ea41110734fb14e88) | Optional desktop settings UI: Core, GUI, Widgets | [LICENSES](qtbase/LICENSES) |
| [qttools](https://github.com/qt/qttools) | 6.8.4-lts-lgpl | [`fd835497a2c0`](https://github.com/qt/qttools/commit/fd835497a2c0d441ea08a35b09b6abd2b5c29b48) | Optional Qt translation and development tools | [LICENSES](qttools/LICENSES) |
| [qttranslations](https://github.com/qt/qttranslations) | 6.8.4-lts-lgpl | [`ba2badad1dcb`](https://github.com/qt/qttranslations/commit/ba2badad1dcb34f0755bc79a27da0485991c5a77) | Optional Qt UI translations | [LICENSES](qttranslations/LICENSES) |
| [tracy](https://github.com/wolfpld/tracy) | 0.14.1 | [`30997d5ca6bb`](https://github.com/wolfpld/tracy/commit/30997d5ca6bb632cc10807a1da8a6d3de0aeeb3c) | Runtime profiling support | [LICENSE](tracy/LICENSE) |

## Selection and integration notes

- The decomp pin is a published development commit. It is incomplete and not
  fully linked. No 1.0 or complete-decomp release is assumed to exist.
- Aurora dependency versions follow the declarations in its pinned
  `extern/CMakeLists.txt`, `cmake/AuroraDependencyVersions.cmake`, and test setup.
  Corrosion follows nod's pinned CMake requirement. Qt components use the same
  publicly available 6.8.4 source-release family.
- Dawn and RmlUi use the compatibility forks selected by Aurora, under
  `encounter/dawn` and `encounter/RmlUi`. Their original projects are
  [Dawn](https://dawn.googlesource.com/dawn) and
  [RmlUi](https://github.com/mikke89/RmlUi). Preserve their upstream history and
  notices; changes made for this port belong in our patch series.
- SQLite's upstream Git source needs its amalgamation generated before supplying
  Aurora's expected `sqlite3.c`/`sqlite3.h` inputs. That build step is not yet
  implemented. A Git checkout is not interchangeable with an amalgamation ZIP.
- FreeType's Git source may require its normal source-generation tools. The
  archive-only download used by Aurora must be redirected to prepared local
  source during integration.
- Aurora already contains a THP decoder and its card implementation. A separate
  FFmpeg or kabufuda checkout is not required by the selected Aurora graph.
- The Qt modules provide a possible separate desktop settings application;
  Dear ImGui and RmlUi cover runtime UI options. No UI backend has been selected
  as a shipping requirement yet.

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
integration must deliberately select the prepared local providers, reconcile
Dawn's own Abseil/GoogleTest dependencies, and avoid unrecorded moving downloads.
The current bootstrap does not fetch additional Git repositories while building.

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
