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
check. Wii disc reading already works separately through nod; connecting it
to game file requests is later work. The core check writes its own local data
under `build/aurora/aurora-check-data/`, without changing personal game settings.

The [Aurora patch series](../patches/README.md#aurora-series) keeps optional GX/
RmlUi dependencies out of a core build, preserves the parent's testing setting,
declares the core/VI/OS link dependencies, and implements MEM2 allocation with
arena/heap cleanup at shutdown. The core diagnostic leaves MEM2 disabled;
the startup checks below exercise it. Upstream submodule contents stay unchanged.

## Experimental original startup

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
  -> STOPPED: glplatPreStartup (GX graphics)
```

The patch series extracts the unchanged region/language and `nlInit()` prefix
from `Game/main.cpp` into `Game/Startup.cpp`. The original `Initialize()` also
calls this prefix; the prototype links it independently while the remaining
game sources are unfinished. Host PowerPC GQR initialization is deferred to the
explicit `SAnimInitGQR` failure point until native animation decoding exists.

The current stop is an actual call from the original `nlInit()` immediately
after memory initialization. GX startup, the original NL asynchronous file
service, and native paired-single animation setup remain explicit typed failure
points. They throw a diagnostic and are linked only in this opt-in build.
Exit code **3** means an expected development stop; **1** means a configuration,
disc, or host error, and **2** means invalid command-line usage. The diagnostic
log is `<executable-directory>/startup-data/startup.log`.

This uses Aurora's null backend. It does not render the game, enter its task
loop, load boot assets, or reach a menu or match. Display/audio/gameplay input
preferences are not applied yet. Explicit English/French/Spanish preferences
execute the original USA language branch; `auto` currently falls back to English.
Other regions/revisions are rejected for this runtime even if disc inspection
succeeds. Startup reads the personal INI without changing it; the launcher only
writes settings when saved or when **Try startup** saves pending changes.

For the next source inventory step, compile the full patched original entry
translation unit without linking or executing it:

```sh
cmake --build build/startup --target charged_game_scan
```

This writes `build/startup/game-entry-undefined.txt` using the toolchain's `nm`.
These are references from one object, including C++ library and already provided
symbols; this is not a complete missing-definition inventory or a game link check.

CTest adds four suites to the Aurora preset, for ten total:

- `startup_headers`: native scalar widths and chunk pointer alignment.
- `native_allocator`: the patched original allocator's alignment, exhaustion,
  mixed allocations from both ends, payload preservation, and coalescing.
- `runtime_memory`: real MEM2 arenas, original memory initialization, SDK heap
  allocation, frees routed to their owning game arena, and repeated cleanup.
- `game_startup`: the original initialization prefix with synthetic Wii data,
  regional language selection, the explicit GX stop, and input errors.

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
UBSan checks. An owned USA revision 1 RVZ reaches the GX stop after reporting
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

1. **Dawn and Vulkan:** prepare the pinned Dawn dependency graph, including its
   required nested sources. Reconcile its Abseil and GoogleTest versions with
   Aurora's providers; build the required Linux backend with other platform
   backends disabled. Provide the remaining pinned image/font/cache libraries
   and generate SQLite's amalgamation from its source submodule. Create a GX
   test scene using original geometry and textures. Check drawing, resize,
   presentation, device errors, and shutdown before claiming graphics support.
2. **Game data access:** connect Wii partition/FST reads to the game's file API.
   Test asynchronous reads, bounds, error propagation, paths, and endianness using
   synthetic fixtures; then compare reads with locally supplied game data.
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
