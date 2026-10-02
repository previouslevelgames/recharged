# Runtime development before the complete decompilation

Much of the host integration can be developed and checked while the game source
is incomplete. Keep these checks independent of the full game link and record
missing behavior explicitly. An initialized window is not evidence of working
gameplay or Wii compatibility.

## Aurora core initialization

The optional `aurora` preset builds the launcher, bootstrap, and a separate
`mscharged-aurora-check` tool. It prepares the pinned Aurora, SDL, Abseil, fmt,
xxHash, and Tracy sources using the same export/patch/verify pass as the decomp.
It provides these libraries to Aurora before configuration, avoiding system
substitutions and additional source downloads. C++20 is required for Aurora.

```sh
git submodule update --init --checkout extern/aurora extern/abseil-cpp extern/fmt extern/xxhash extern/tracy
cmake --workflow --preset aurora
./build/aurora/mscharged-aurora-check --window
```

The normal launcher dependencies must also be initialized as described in
[BUILDING.md](BUILDING.md). Close the check window to exit. Without `--window`,
the tool runs bounded checks and exits automatically; CTest runs this mode
with SDL's dummy video and software rendering drivers, requiring no game data.

This exercises Aurora's initialization/shutdown, window/event loop, controller
subsystem initialization, MEM1 boot information and arena allocation, and
advancing/paused clocks. The window displays diagnostic text using Aurora's
SDL renderer. GX is explicitly disabled, so this does **not** test Dawn, Vulkan,
shaders, game rendering, complete Wii memory behavior, or controller mappings.

Aurora's DVD, card, THP, and RmlUi components are also disabled in this first
check. The separate startup preset below connects DVD/nod to original NL reads.
The core check writes its own local data
under `build/aurora/aurora-check-data/`, without changing personal game settings.

The [Aurora patch series](../patches/README.md#aurora-series) keeps optional GX/
RmlUi dependencies out of a core build, preserves the parent's testing setting,
declares the core/VI/OS link dependencies, and implements MEM2 allocation with
arena/heap cleanup at shutdown. The core diagnostic leaves MEM2 disabled;
the startup checks below exercise it. Upstream submodule contents stay unchanged.

## Independent GX/Vulkan diagnostic

The Linux-only `graphics` preset builds `mscharged-gx-check` with real Aurora
GX and Dawn Vulkan. It builds the bootstrap alongside it;
the launcher and original startup use separate presets. A C++20 compiler,
GNU Make, Tcl 8.6+, Vulkan loader/driver, and a desktop session are required.
Install the Vulkan validation layers for the diagnostic's backend validation.

Initialize the used top-level sources and the selected nested sources:

```sh
git -c submodule.recurse=false submodule update --init --checkout extern/mscharged-decomp extern/nod extern/corrosion extern/sdl extern/imgui extern/aurora extern/dawn extern/fmt extern/xxhash extern/tracy extern/zlib-ng extern/libpng extern/freetype extern/sqlite extern/zstd
git -C extern/dawn -c submodule.recurse=false submodule update --init --checkout --depth 1 --jobs 3 -- third_party/abseil-cpp third_party/jinja2 third_party/markupsafe third_party/spirv-headers/src third_party/spirv-tools/src third_party/vulkan-headers/src third_party/vulkan-utility-libraries/src
git -C extern/freetype -c submodule.recurse=false submodule update --init --checkout --depth 1 -- subprojects/dlg
cmake --workflow --preset graphics
./build/graphics/mscharged-gx-check --window
```

The first build compiles Dawn and its shader compiler and can take considerably
longer than a launcher build. Limit parallel compilation on machines with less
memory, for example `CMAKE_BUILD_PARALLEL_LEVEL=4 cmake --workflow --preset graphics`.
Without `--window`, the check renders 180 frames and exits. `--resize-test`
also requests a window resize after frame 60; `--frames N` changes the bound.

The scene contains a colored triangle and a generated, tiled GX RGBA8 checker
texture. It uses vertex colors, projection/model matrices, one TEV stage, depth
testing, and a depth readback. It requires the actual Vulkan backend, draw calls,
the expected triangle depth, and no logged backend errors. No game data or personal
INI is read. Local GPU/cache data goes under `build/graphics/gx-check-data/`.
This is an independent rendering check; the `startup` preset uses the null
backend and does not execute original game graphics initialization.

The ordinary graphics workflow runs six suites without requiring a GPU,
including four generated-SQLite lifecycle cases.
To also register the desktop Vulkan check with CTest:

```sh
cmake --preset graphics -DMSCHARGED_TEST_VULKAN=ON
cmake --build --preset graphics
ctest --preset graphics
```

Those two additional suites need a working desktop and Vulkan device. One checks
the default validated device with resize, and one checks Release device
optimization flags. Both force the system Vulkan validation layer on and
reject raw Vulkan validation errors as well as nonzero exits. The tool rejects
a fallback backend or CPU adapter. A dummy SDL window cannot pass them.
`--optimized-device` selects the second device configuration for manual checks;
ordinary diagnostic runs keep API validation and robustness enabled.

Linux validation on an Intel UHD Graphics 620 with Mesa 26.2.4 passed both GPU
suites, fresh and warm cache runs, resize, and shutdown. The generated scene was
also inspected, including all 16 checker cells after resize. A later forced X11
run stalled in SDL's window-manager acknowledgement; the normal desktop path
continued to pass. X11 startup reliability remains unqualified.
The warning about `DAWN_ENABLE_VULKAN_VALIDATION_LAYERS` refers to bundled layer
paths; this build uses the installed system validation layer.

Dawn's nested Abseil is the single shared provider for Dawn and Aurora in this
preset. Upstream tests are disabled, so neither GoogleTest checkout is built.
The seven selected Dawn gitlinks, their recursive pins, and the selection itself
are recorded in the preparation manifest. Optional browser, compiler-toolchain,
other-platform, and upstream-test sources are omitted. No gclient sync or
build-time source fetching is enabled.
The Dawn version generator receives its verified source pin explicitly, avoiding
accidental discovery of the parent port repository's Git revision.

PNG, zlib-ng, FreeType, Zstandard, SQLite, and ImGui come from prepared pinned
sources. SQLite's amalgamation is generated in an isolated build directory by
`tools/prepare_sqlite.py`; its manifest records the source key, generator hash,
host compiler/Make/Tcl versions, and generated contents. A check before
compilation rejects stale or edited outputs. Python generator bytecode stays
out of prepared sources through the [Dawn patches](../patches/README.md#dawn-series).

Broader GX formats/state, fullscreen/DPI, device recovery, and cache invalidation
across upgrades remain integration work. Windows/macOS graphics are unverified.

## Experimental static Wii asset preview

The Linux `scene` preset brings GX/Dawn Vulkan and DVD/nod into the same
`mscharged` executable, sharing one pinned ImGui implementation with its SDL
launcher. The common selected original core/allocator/NL services are defined
in `cmake/NativeRuntime.cmake`. The normal `startup` preset retains its null
backend and explicit remaining-initialization stop.

Initialize the same sources as the graphics preset above, then:

```sh
CMAKE_BUILD_PARALLEL_LEVEL=4 cmake --workflow --preset scene
./build/scene/mscharged --experimental-scene --config ./mscharged.ini
```

The first preview reads `/Art/objects/gameplay/ball.rlg` and `ball.rlt` directly
from your USA `R4QE01` revision 1 image. It enters original `InitializeCore`,
queues both files through `nlLoadEntireFileAsync`, services NL on the main
thread while processing window events, and adopts/frees the buffers using the
original game allocator. It runs the original `PreInitFS` memory callback and
installs checked geometry/textures into pool-owned native `glModel` and
`PlatTexture` records. Original `GLInventory` lookup and the static texture
manager supply the renderer. It draws those records through GX and
Aurora's real Vulkan backend. Nothing is extracted or written back to the disc.

**This is a static asset preview.** It has a host camera and slow rotation,
with one diffuse-only TEV stage. Original material effects, lighting, animation,
the full `glStartup`, and scene/task code are not executed. Display,
audio, and control preferences do not configure this preview. The first model
is selected by default; alternate RLG models are not drawn over it.

Escape or close the window to exit. For a bounded run or a supported alternate
static model/bundle, use:

```sh
./build/scene/mscharged --experimental-scene --config ./mscharged.ini --frames 180
./build/scene/mscharged --experimental-scene --config ./mscharged.ini --model-id 0x226798ba
```

`--model /DISC/PATH.rlg` and `--textures /DISC/PATH.rlt` override the pair using
absolute Wii data-partition paths. Unsupported data fails explicitly; these
options do not imply support for every model. Running without the experimental
flag opens the normal launcher. The preview is absent from normal Debug/Release
builds and from the null-backend startup build. All diagnostic/cache files stay
under `<executable-directory>/scene-data/`, including `scene.log`. The INI is
never saved by direct preview startup.

### Checked static asset profile

`src/resources/` separates fixed-width Wii records from native objects:

- Static RLG groups/collections, 12-byte model records, 48-byte packet records,
  8-byte stream records, 16-bit indices, and big endian 4x4 affine matrices.
  Matrices follow the original NL-to-GX transpose convention and are baked once.
  Position streams use float triples. Supported material profiles use float
  UVs or signed 16-bit UVs with ten fractional bits. Normals/colors/additional
  UVs have checked ranges but are unused by the diffuse preview.
- MaskedSpecularFresnel, ScrollingDiffuse, UnlitTexture, and VertexColourTexture
  parameter layouts are recognized only to find their first diffuse binding.
  Their original shaders, raster-state behavior, scroll parameters, specular
  masks, and vertex lighting/color modulation remain unimplemented here.
- Static RLT dictionaries use offsets relative to the data block. Their metadata
  is decoded; GX-tiled pixels and RGB5A3 palette words retain Wii byte order for
  Aurora to upload. The reader covers RGB565, RGB5A3, CMPR, RGBA8, I8/I4/A8,
  IA8, and CI8 physical tile sizes, mip levels, and palette bounds. This is
  reader coverage; GPU output for every format is not established.
- Skinning, vertex animation, animated textures, unknown chunks/programs,
  missing diffuse bindings, malformed indices/offsets, nonfinite coordinates,
  and unsupported matrices fail rather than producing placeholder resources.
  Each input is limited to 16 MiB. Decoded geometry and aliased texture copies
  have separate cumulative budgets to bound memory use.

The host decoder arrays are discarded after installation. Native headers and
indices use the original MEM1 resource pool; vertex/tiled texture/palette bytes
use its MEM2 pool. The diffuse profile creates position and float UV streams
and a texture binding, without inventing a material program or display list.
Aurora GX objects are constructed separately, preserving their actual host
size; the Wii-sized object arrays in `PlatTexture` are not reinterpreted.

### Original graphics memory and static inventory

`charged_graphics_memory` selects original GL/GLX memory, AVL tree, and
`PreInitFS` code, plus explicit static inventory/texture-manager entry units.
It retains the original 512 KiB MEM1 and `0x233333`-byte MEM2 frame budgets,
3 MiB texture + 2 MiB vertex + 2.25 MiB header resource requirements, and
1,000 texture slots. Both frame-buffer halves have aligned starts; padding
does not enlarge their logical allocation limits. Addresses and marker handles
use pointer-sized types. Failed allocations leave offsets and selected allocators
unchanged and reclaim partially initialized pools/containers.

A resource marker owns each installed batch. Releasing it removes nested
inventory entries, returns texture indices, frees owned file/linear buffers,
and rewinds pool storage. The static subset selects file/model/texture
containers only. Animation, skin and chunk-loader methods remain unlinked;
their container pointers are null, with no successful replacement functions.
Existing texture bindings resolve again after a resource rollback or shadowing.
Resource markers are lifetime-bound handles and must not be reused after release.

The preview advances original frame memory and uses it for its camera matrices.
Frame advancement requires an installed cache invalidator; the preview supplies
real `GXInvalidateVtxCache` / `GXInvalidateTexAll` calls. Without that provider
it stops explicitly. CPU-only tests inject an observed invalidation callback;
they do not establish GPU behavior. Shutdown drains GX commands before resource
rewind, destroys inventories before pool backing memory, then destroys the
texture manager and frame buffers. Pending reads are cancelled/drained before
disc/arena teardown. There is no persistent native asset cache.

### Verification and next original scene work

Linux verification with the owned RVZ loaded the 40,256-byte mesh and
60,256-byte texture bundle. Model `0x8ba9ca19` has one packet, 450 vertices,
819 indices; the bundle has four textures. Its visible CMPR diffuse texture
and geometry were inspected in a 600-frame run. The bounded check requires
actual GX draw calls, no logged backend errors, and a geometry depth sample
within the perspective camera's range. Aurora's depth readback is asynchronous;
its initial zero is never counted as geometry.

The portable `static_resources` suite checks synthetic format/byte-order,
alignment, malformed/truncated-input, and allocation-budget cases. With
`MSCHARGED_TEST_VULKAN=ON`, `scene_synthetic` additionally renders synthetic
assets from a generated Wii ISO and checks missing assets/bindings/model IDs,
wrong revisions, CLI mode conflicts, and unchanged personal INI. It uses the
installed validation layer, like the two independent GX suites. The scene
preset has nine portable suites and three opt-in GPU suites. `graphics_memory`
also runs in the startup preset, covering native addresses, double-buffer
alignment, MEM1/MEM2 and partial-construction failures, nested/shadowed inventory
entries, original AVL lookup/release, texture exhaustion/recycling, and repeat
initialization/shutdown. `static_inventory` checks native resource ownership,
palette-byte preservation, drain-before-release and conversion/OOM rollback.
Targeted ASan/UBSan/leak checks cover the selected original memory/inventory/
texture/AVL units, native adapters and checked readers. Aurora/Dawn/nod and
remaining dependencies retain ordinary builds. This does not establish
full game resource ownership, Wii presentation parity, or other platforms.

For an original game scene, next reconstruct the required material programs
and their native parameters, adapt original loaders to the checked native
records, then enable platform/state/matrix/view/target/font setup and the
original frame/task loop. The independent preview keeps those missing services
visible while providing a real asset to check conversions and GX behavior.

## Experimental original startup

The selected decomp snapshot is
[`45f25bd65195`](https://github.com/yannicksuter/mscharged-decomp/commit/45f25bd6519586837e61251208d34d8dcf94e170).
Upstream now marks `Game/main.cpp` as matching for the original Wii build.
Its startup prefix is unchanged from the previous port baseline. The full
patched entry compiles as a native reference object; the prototype below links
only the enabled initialization subset. Matching one unit does not supply its
other game definitions, native services, or resource conversions.

The opt-in `startup` preset adds original game initialization to the same
`mscharged` executable. It requires the dependencies and C++20 compiler used by
the `aurora` preset, and enables Aurora DVD using the existing prepared nod
library. From the repository root:

```sh
git submodule update --init --checkout extern/mscharged-decomp extern/nod extern/corrosion extern/sdl extern/imgui extern/aurora extern/abseil-cpp extern/fmt extern/xxhash extern/tracy
cmake --workflow --preset startup
./build/startup/mscharged
```

Select your USA `R4QE01` revision 1 ISO/RVZ and wait for the disc check. **Try
startup** saves pending settings, closes the launcher, and starts the prototype.
To enter it directly with your existing configuration:

```sh
./build/startup/mscharged --experimental-startup --config ./mscharged.ini
```

Direct startup is verified on Linux. The launcher action is implemented; its
complete handoff still needs verification.

The current path is:

```text
Aurora core / MEM1 / MEM2 / clocks
  -> Aurora DVD opens the Wii data partition
  -> original InitializeCore(): region and game text language
  -> original nlInit()
  -> original nlInitMemory(): game allocators and reserved SDK heap
  -> original glplatPreStartup(): returns true at this source pin
  -> original ticker / time / random initialization
  -> original nlInitFileSystem(): Aurora DVD-backed NL reads
  -> SAnimInitGQR(): native decoders encode fixed quantization constants
  -> original nlInit() / InitializeCore() return
  -> diagnostic NL synchronous / asynchronous disc reads
  -> diagnostic native animation key decoding
  -> diagnostic whole-file async reads of the two boot INIs
  -> STOPPED: Initialize (remaining stages are not linked)
```

The patch series extracts the unchanged region/language and `nlInit()` prefix
from `Game/main.cpp` into `Game/Startup.cpp`. The original `Initialize()` also
calls this prefix; the prototype links it independently while the remaining
game sources are unfinished. Native rotation decoders now encode the fixed
PowerPC GQR behavior directly; the native `SAnimInitGQR()` has no register state
to initialize. The original unsigned scale and byte-weight decoders compile
unchanged from prepared `Game/SAnimDecode.cpp`.

The unchanged `glplatPreStartup()` helper is extracted and linked independently;
actual GX/VI initialization is in the later `glplatStartup()` function. The
prototype now runs original `nlFileGC.cpp` and the extracted basic operations
from `nlFile.cpp`, using Aurora's native DVD types rather than casting Wii SDK
layouts. Explicit synchronous and asynchronous prefix reads of a real disc file
are compared, including their destination boundaries and caller-thread callback.
The log records the path, byte count, and prefix hash, without saving game bytes.
Files and pending worker writes are drained before disc closure and arena release.

Enabled operations include path lookup, logical/padded size, seek, synchronous
read, raw asynchronous head/tail reads, service/cancel/close, and synchronous
whole-file loading through the standard/virtual game arenas or supplied buffers.
The port adapter also implements asynchronous whole-file loading and cancellation.
Bounds, capacity, allocation, short-read, and DVD errors fail explicitly.
NL submission/service/cancel runs on the calling game thread; only Aurora's DVD
worker performs background I/O. The original padded-buffer contract can read and
report up to the next 32-byte boundary while advancing the logical cursor by the
requested size. Exact buffers use tail scratch storage instead. Callback context
is pointer-sized; the native profile rejects files outside the signed DVD read
range, including padding.

Decompression, bundles/caches, custom-heap
ownership, priority/retry parity, and host-file loading remain outside this
enabled subset. Host-file/custom-heap requests fail explicitly; other unselected
APIs are not supplied as successful stubs. Callback functor storage uses explicit
game-arena allocation; the original function-pool state API is not linked.
The remaining original `Initialize()` stages retain an explicit diagnostic stop.
Further graphics initialization is not executed.
Exit code **3** means an expected development stop; **1** means a configuration,
disc, or host error, and **2** means invalid command-line usage. The diagnostic
log is `<executable-directory>/startup-data/startup.log`.

This prototype uses Aurora's null backend. It does not render the game, enter its task
loop, decode boot assets, or reach a menu or match. Display/audio/gameplay input
preferences are not applied yet. Explicit English/French/Spanish preferences
execute the original USA language branch; `auto` currently falls back to English.
Other regions/revisions are rejected for this runtime even if disc inspection
succeeds. Startup reads the personal INI without changing it; the launcher only
writes settings when saved or when **Try startup** saves pending changes.

### Native whole-file asynchronous loading

`src/runtime/whole_file.cpp` implements `nlLoadEntireFileAsync` and
`nlCancelEntireFileLoad` over the original prepared raw NL manager. The original
advanced `nlFile.cpp` implementation remains unselected. Patch 0020 connects
failed-read and shutdown cleanup to the port adapter, including reads serviced
inside synchronous `nlRead` calls.

The existing game API keeps its 32-bit handle fields. Native handles are opaque
nonzero tokens, with no pointer casts or reuse across shutdown. Callback state
uses pointer-sized context, and bookkeeping uses the host allocator.

| Case | Native behavior |
| --- | --- |
| Missing file | Return 0; no callback |
| Empty file | Inline callback with null/0; return 0 |
| Nonempty file | Queue a read; callback on the NL servicing thread with the logical size |
| Allocated output | Standard/virtual arena allocation with requested alignment/end; ownership transfers at callback entry, including exceptions |
| Supplied output | Always borrowed; capacity 0 means the caller guarantees the logical length |
| Submission/read error | Throw, drain the failed request and release owned storage; no success callback; unrelated requests survive |
| Cancellation | Join pending workers before the cancel callback; it borrows the buffer and receives the original completion callback; release only owned data |
| Shutdown | Drain all pending whole-file requests before manager/arena release; no callbacks |

Cancellation also drains a busy native worker, matching the raw native adapter.
This deliberately differs from the console implementation's refusal to cancel
busy reads: Aurora submits host reads immediately. Cancellation can therefore
wait for in-progress I/O. Completed, cancelled, and unknown tokens return false;
handle exhaustion fails explicitly. NL calls remain confined to the game thread.
Callbacks must free allocated output with `nlFree`, including on exceptions.
Cancel callbacks must not free the borrowed buffer or retain it after returning.

The startup diagnostic reads `ini/common.ini` and `ini/datetime.ini` both
synchronously and through this API, compares every byte and logs lengths/hashes.
It rejects missing, empty, or oversized boot INIs and saves no game bytes. This
does not execute the original config parser, tweak registration, or any later
game initialization. Those INI consumers, compressed loads, bundles and caches
remain pending.

### Native animation key decoding

`charged_sanim_decode` links prepared `Game/SAnimDecode.cpp` and the port's
`src/runtime/sanim_decode.cpp`. Patch 0019 retains console assembly for console
builds and selects scalar native rotations. GQR6 is `0x0f070f07` (signed 16-bit,
scale 15); GQR7 is `0x07060706` (signed 8-bit, scale 7). Dequantization multiplies
the signed integer by `2^-scale`, using the original constants and the
[IBM Gekko manual](https://doc.kodewerx.org/documents/gekko_user_manual.pdf),
sections 2.1.2.9 and 2.3.4.3.12.

| Entry | Input contract | Output |
| --- | --- | --- |
| `SAnimDecodeRot16` | 8 big-endian bytes, four signed 16-bit components | Components divided by 32,768 |
| `SAnimDecodeRot12` | 6 packed bytes, four signed 12-bit components | Original left-shift/unpack behavior, equivalent to division by 2,048 |
| `SAnimDecodeRot8` | 4 bytes, four signed 8-bit components | Components divided by 128 |
| `SAnimDecodeScale` | Aligned host-order `PackedScale`, three **unsigned** 16-bit fields | Components divided by 2,048 |
| Weight / morph weight | One unsigned byte | Value divided by 255 |

Packed rotations support unaligned keys, retain Wii byte order, and are not
normalized. Scale data must be converted by the asset loader before constructing
the host-order struct; these entry points do not relocate animation chunk
pointers or convert translation/root/morph metadata. Input and output must not
overlap. Animation chunk loading, pose blending, interpolation, and original
animation playback remain unselected.

`sanim_decode` tests every component value in every lane, original mixed-sign
fixtures, packing/byte order, output guards, input preservation, and repeated
initialization. Startup also runs bounded synthetic keys through the actual
linked entry points. These checks verify scalar decoding against the original
format/register contract; they do not establish console hardware or gameplay
animation parity.

For the next source inventory step, compile the full patched original entry
translation unit without linking or executing it:

```sh
cmake --build build/startup --target charged_game_scan
```

This writes `build/startup/game-entry-undefined.txt` using the toolchain's `nm`.
These are references from one object, including C++ library and already provided
symbols; this is not a complete missing-definition inventory or a game link check.
The current scan contains 235 references (234 strong undefined and one weak),
with five incomplete inline virtual declarations in the decomp headers.

After `nlInit()`, original `Initialize()` still requires `glStartup(PreInitFS)`,
Wii input, the tweak registry, `art/global.rlt`, asynchronous configuration and
`ini/datetime.ini`, task/audio/localization/front-end initialization, and save/
file-cache services. Original `main()` then waits for pending reads, selects
the initial task state, enables front-end music, and runs `nlTaskManager` forever.
Native integration must connect those dependencies and provide event handling
and shutdown before using that complete entry as a playable runtime.

The scene preset independently executes `PreInitFS` with patched native pools
and the selected static inventory/texture manager. The null-backend startup
prototype still stops before `glStartup`. Its remaining graphics graph needs
material registries, platform GX/VI, and state/matrix/target/view/font setup,
followed by native loaders and the original frame/task integration.

CTest adds seven suites to the Aurora preset, for fourteen total:

- `sanim_decode`: exhaustive packed rotation components, unsigned scales and
  weights, with independent bit-stream encoding and exact float comparisons.
- `startup_headers`: native scalar widths and chunk pointer alignment.
- `native_allocator`: the patched original allocator's alignment, exhaustion,
  mixed allocations from both ends, payload preservation, and coalescing.
- `runtime_memory`: real MEM2 arenas, original memory initialization, SDK heap
  allocation, frees routed to their owning game arena, and repeated cleanup.
- `graphics_memory`: CPU checks of original frame/resource allocation,
  selected static inventory, texture-index ownership, failure cleanup and the
  original `PreInitFS` budgets; no GPU session is created by this suite.
- `runtime_files`: synthetic Wii files/directories and known payload bytes;
  exact/padded/unaligned reads, seek/EOF and capacity errors, 64-bit callback
  context, caller-thread delivery and reentrancy, cancellation during active
  I/O, callback exceptions, bounded pool exhaustion, whole-file arena ownership,
  short/failed reads, and repeated initialization/shutdown. Async whole-file
  cases cover arena/supplied ownership, empty/missing files, queue/OOM rollback,
  recursive reads, callback exceptions, nested cancellation, stale tokens,
  read-error isolation and active-worker shutdown.
- `game_startup`: the completed original core initialization with synthetic Wii data,
  real NL reads, whole-file boot INI bytes and missing/empty errors, regional
  language selection, decoder checks, the explicit
  remaining-initialization stop,
  unchanged personal settings, and input errors.

No proprietary game data is needed for those checks.

### Original memory initialization

The prototype allocates real 24 MiB MEM1 and 64 MiB MEM2 buffers through Aurora.
The patched original `nlInitMemory()` constructs its standard and virtual game
allocators and creates the reserved heap using Aurora's existing SDK allocator.
The original reserve sizes and initialization order are retained. MEM2 arena
allocation rejects invalid alignment and overflowing requests.

The original free-list allocator now uses native pointer arithmetic, the actual
free-block structure size, and its native alignment. It retains relative 32-bit
allocation trailers, allocation from either end, and free-block coalescing.
Explicit `nlMalloc`/`nlFree` calls use these game arenas; freeing selects the
owner by its range instead of Wii address bits. Native exhaustion throws
`std::bad_alloc`. Shutdown clears game and SDK heap state before releasing
Aurora's memory, allowing the tested memory initialization to repeat.

Linux checks cover 64 MiB and 128 MiB MEM2 configurations, including the
original 128 MiB reserve branch. The standalone allocator also passes ASan and
UBSan checks. An owned USA revision 1 RVZ completes the original core and reaches
the remaining-initialization stop after reporting
25,148,416 bytes in the MEM1 game arena and 67,100,672 bytes in the MEM2 game
arena. Gameplay and console-layout parity remain unverified.

Host global `new`/`delete` remain supplied by the C++ runtime. Routing ordinary
game and class-specific allocations, retaining ownership after custom allocators
leave the stack, thread safety, and used Wii physical/cached/uncached address
translations are still integration work. Full game shutdown must destroy live
objects and drain pending work before releasing arenas. Windows/macOS memory
behavior has not been verified.

## Graphics backend policy

The launcher only offers game backend preferences appropriate to its build:

| Platform | Planned game choices | Automatic preference |
| --- | --- | --- |
| Linux | Automatic, Vulkan | Vulkan |
| Windows | Automatic, Direct3D 12, Vulkan | Direct3D 12 |
| macOS | Automatic, Metal | Metal |

These are saved preferences pending GX integration. GPU/driver availability
must be checked at runtime before reporting a usable backend. An unavailable
explicit selection should produce a useful error, rather than silently reporting
success with a different renderer. An INI copied from another OS may retain an
unsupported backend; the launcher asks the user to choose an available entry.

The launcher itself uses SDL rendering, independently of the future game's
Aurora/Dawn backend. Metal is not offered by Linux builds. OpenGL/OpenGL ES and
additional backends are not promised by this initial policy.

## Next independent milestones

1. **GX coverage and game graphics:** extend the independent Vulkan scene to the
   GX operations actually used by Charged. Qualify presentation, DPI/fullscreen,
   device failures, and cache upgrades; then connect original graphics startup.
2. **Game data access:** extend the verified original raw NL reads to whole-file
   async, decompression, bundle/cache ownership, and boot-resource waits. Preserve
   error/cancel behavior and compare ISO/RVZ reads of the same locally supplied
   disc; byte reads alone do not validate native resource layouts.
3. **Wii host services:** extend the verified startup allocator to remaining
   game allocation and address interfaces; define native pointer boundaries,
   timing, threads, callbacks, saves, and configuration. Map supported game text
   languages to the original Wii system-language path. GameCube card and PAD
   implementations alone do not establish Wii save or WPAD/KPAD support.
4. **Input and audio:** specify Wiimote/Nunchuk actions and controller/keyboard
   mappings, including pointer and motion behavior. Check device hotplug and
   rumble independently. Inventory the game's RVL AX calls and build bounded
   audio tests with generated signals before integrating game playback.
5. **Incremental game code:** expand the explicit native source list as units
   become usable. Classify compile/link failures, fix ABI/endian issues through
   reviewable patches, and bring up resource loading before menu/gameplay work.
   Keep missing functions visible; avoid success stubs that conceal them.

Each milestone should have an independent, runnable check. The eventual 100%
decompilation and 100% link release becomes a new reviewed upstream baseline;
it does not automatically replace the port's current pin or prove native parity.
