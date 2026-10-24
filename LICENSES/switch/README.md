# Switch build notices

The Switch program links these libraries statically from the pinned devkitPro
image (`tools/switch/toolchain.env`), which ships no notices for them:

| Library | License | Text |
| --- | --- | --- |
| libnx, the Switch system library | ISC | `libnx.txt` |
| SDL2 2.28.5 (devkitPro's `switch-sdl2`), under sdl3on2 | zlib | `SDL2.txt` |
| libdrm_nouveau (devkitPro's `switch-libdrm_nouveau`) | MIT | `libdrm_nouveau.txt` |
| Mesa's EGL and OpenGL ES libraries (devkitPro's `switch-mesa`), used by SDL2 | MIT | Mesa's notices, with mesa-switch's in `LICENSES/dependencies/mesa-switch` |
| bzip2 1.0.8 (devkitPro's `switch-bzip2`), for bzip2 RVZ and WIA images | bzip2 license (BSD-style) | `bzip2.txt` |
| liblzma 5.8.2 from XZ Utils (devkitPro's `switch-liblzma`), for LZMA RVZ and WIA images | 0BSD | `liblzma.txt` |

The disc reader (`src/platform/wii_disc`) reads discs in nod's place and keeps
nod's C API header: nod's MIT license, from its pinned `extern/nod`, is `nod.txt`.
Its RVZ and WIA support follows nod's reader. Zstandard, its third codec, is the
prepared `zstd` source dependency, with its notices in `LICENSES/dependencies`.

The NVK Vulkan driver is built from mesa-switch (`tools/switch/build-nvk.sh`); its
`NOTICE-NVK.md` covers Mesa, expat, zstd and NVK's Rust runtime.
