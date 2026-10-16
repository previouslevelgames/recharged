# Dependencies

Every source dependency is a Git submodule pointing to its upstream
repository. The commit recorded in this repository (the gitlink) is the exact
pin; `git submodule status` lists them. No build follows an upstream branch:
a new version enters only through a reviewed update of the pin and its
[patches](../patches/README.md).

`python3 tools/setup_dependencies.py` initializes only the submodules the
chosen build needs, at their recorded commits (`--launcher` for the smaller
launcher-only build). The build then prepares patched copies of them under
`build/`; the checkouts here stay unchanged.

| Dependency | Version | Used for | License |
| --- | --- | --- | --- |
| [mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp) | development snapshot | The game's reconstructed source code | CC0 for original contributions, plus third-party notices |
| [Aurora](https://github.com/encounter/aurora) | development snapshot | GameCube/Wii platform layer: GX graphics, video, DVD | MIT |
| [Dawn](https://github.com/encounter/dawn) | Aurora's pin | WebGPU on Vulkan, DirectX 12 and Metal | BSD |
| [SDL](https://github.com/libsdl-org/SDL) | 3.4.10 | Window, audio output, gamepads, launcher rendering | zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.91.9b docking | Launcher interface (its Roboto font is Apache 2.0) | MIT |
| [nod](https://github.com/encounter/nod) | 2.0.0-alpha.12 | Reading ISO and RVZ disc images | MIT or Apache 2.0 |
| [Corrosion](https://github.com/corrosion-rs/corrosion) | 0.6.1 | Building nod's Rust code from CMake | MIT |
| [FreeType](https://github.com/freetype/freetype) | 2.14.3 | Font rasterization | FreeType License (selected) |
| [libpng](https://github.com/pnggroup/libpng) | 1.6.58 | PNG images | PNG |
| [zlib-ng](https://github.com/zlib-ng/zlib-ng) | 2.3.3 | Compression | zlib |
| [Zstandard](https://github.com/facebook/zstd) | 1.5.7 | Cache compression | BSD (selected) |
| [SQLite](https://github.com/sqlite/sqlite) | 3.51.3 | Graphics pipeline cache | Public domain |
| [fmt](https://github.com/fmtlib/fmt) | 12.1.0 | Text formatting in Aurora | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | 0.8.3 | Hashing in Aurora | BSD |
| [Tracy](https://github.com/wolfpld/tracy) | 0.14.1 | Aurora's profiling hooks | BSD |
| [Abseil](https://github.com/abseil/abseil-cpp) | 20240722.0 | Aurora utilities in the host-only check presets (the game build uses Dawn's own copy) | Apache 2.0 |

## Notes

- **Dawn** is used through the fork Aurora selects (`encounter/dawn`; upstream
  is [dawn.googlesource.com](https://dawn.googlesource.com/dawn)). It has its
  own nested submodules; the build initializes only these, at Dawn's recorded
  commits (`cmake/Dawn.cmake`): `third_party/abseil-cpp` (the shared Abseil),
  `jinja2` and `markupsafe` (build-time generators), `spirv-headers` and
  `spirv-tools` (shader tooling), and on Linux and Windows `vulkan-headers` and
  `vulkan-utility-libraries`.
- **FreeType** also needs its recorded `subprojects/dlg` checkout.
- **SQLite** is generated from its Git source by `tools/prepare_sqlite.py`
  (with Make, Tcl and a C compiler); the result is verified before building.
- **nod**'s Rust dependencies are pinned by its `Cargo.lock`; the first build
  downloads those exact crates. Apart from that, nothing is downloaded during
  a build.
- On Windows, the DirectX shader compilers are downloaded by
  `tools/setup_windows_graphics.py` at pinned versions and checksums
  ([`windows_graphics.json`](../tools/windows_graphics.json)).

The license texts are in each checkout, and in every package under
`LICENSES/dependencies/`; see [license scope and notices](../LICENSES/README.md).
