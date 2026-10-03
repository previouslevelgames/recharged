# Upstream patches

Keep upstream checkouts unchanged. Each dependency's patch directory contains:

- `base`: the full commit SHA selected by its submodule gitlink.
- `series`: one relative patch filename per line, in application order. Blank
  lines and lines beginning with `#` are ignored.
- Focused patches explaining the problem, base, and relevant limitations.

The preparation tool checks the base against the recorded submodule pin and
uses `git apply --check` followed by `git apply` for each patch. It rejects
whitespace errors in added lines and does not use automatic three-way merges,
whitespace-ignoring application, or partial reject files. Git may relocate a
hunk with matching context; the exact base pin supplies the version constraint.

Only a completely prepared tree becomes available to CMake. Cache reuse checks
both the inputs and generated contents. A failed new preparation leaves any
previous tree intact, but the build's validation rejects that stale tree.

## Charged series

The current base is `4a9ab8151d9a74e321e52b36c95d1e157d41af60`, adopted from
published `main` on 2026-10-03. The ordered patches are listed in
[`mscharged-decomp/series`](mscharged-decomp/series).
This update refreshes 0005, 0006, 0011, 0012, 0017, 0025 and 0031 for upstream
declaration changes, named allocator fields and allocator helper extraction.
The allocator uses the new shared helpers with native address widths, alignment
and failure checks; startup
and rendering extracts retain their behavior. The remaining patch hunks are
unchanged, with all bases updated to the selected commit.

Upstream now marks both `nlEvent.cpp` and `MemAlloc.cpp` matching. Patch 0038
connects the original immediate event registry and listeners to native startup
checks. Patches 0039–0040 add queued payload cleanup and the original dispatch
task; the complete game task loop remains pending. Matching Wii code and a
strictly applicable patch series do not establish a complete native game;
see [the entry and runtime scope](../docs/RUNTIME.md#experimental-original-startup).

| Patch | Reason | Current validation |
| --- | --- | --- |
| `0001-use-standard-fabs-in-nlMath-header.patch` | The shared math header uses CodeWarrior's undeclared `__fabs`; native Clang rejects `nlRandom.cpp`. Use the standard float overload. | Native compilation; finite, signed-zero, infinity, and NaN checks. |
| `0002-keep-mt-seed-state-32-bit.patch` | Wii `unsigned long` is 32 bits; Linux/macOS LP64 makes it 64 bits. Keep the available MT seed state and public argument explicitly 32-bit. | Signature/type checks and compilation of the available seed implementation. No complete MT generator is present in this subset. |
| `0003-fix-native-rvl-scalar-types.patch` | Keep SDK 32-bit scalars/calendar fields fixed-width; use standard pointer types and avoid host type conflicts. | Startup compilation and scalar/calendar layout assertions. |
| `0004-preserve-native-chunk-addresses.patch` | Align and traverse chunk pointers without truncating host addresses; retain 32-bit serialized size fields. | Native allocation/alignment/traversal checks, including addresses above 4 GiB when supplied by the host. Bounds and endian conversion remain separate work. |
| `0005-include-replay-pose-node-definitions.patch` | Template bodies need complete replay pose types when parsed by host compilers. | Compilation of the original game entry unit. |
| `0006-expose-original-startup-core.patch` | Extract the original region/language/`nlInit()` prefix so it can run before the rest of the game links. Skip PowerPC-only GQR assembly on native builds. | Original USA language branches and real `nlInitMemory()` entry. Animation rotations are connected separately through patch 0019. |
| `0007-keep-native-arena-addresses.patch` | Compute arena capacity from pointers without first truncating their addresses. | Real MEM1/MEM2 startup arena sizes; broader Wii address translation remains pending. |
| `0008-use-standard-memory-header.patch` | Replace four MSL `mem.h` include sites with the host `string.h`. | Native headers and entry compilation; no synthetic `mem.h` shim. |
| `0009-read-native-bus-clock-through-host.patch` | Native timer units obtain the initialized Aurora bus clock through a host adapter. | Compilation/linking of original ticker/time units alongside Aurora. Original game scheduling remains pending. |
| `0010-use-host-placement-new.patch` | Use standard placement new and native allocation-operator argument widths. | Startup, allocator, and original entry compilation. Full game/class-specific allocation remains pending. |
| `0011-adapt-original-free-list-allocator.patch` | Retain the original free-list algorithm with native pointer arithmetic, metadata size/alignment, and explicit invalid-request/OOM errors. | Mixed allocations from both ends, alignment, exhaustion, payload preservation, complete coalescing, and standalone ASan/UBSan checks. |
| `0012-route-native-game-frees-by-arena-ownership.patch` | Route explicit game frees by owning arena rather than console address bits; keep host global new/delete standard. | MEM1/MEM2 allocations freed while a different arena is selected. Ordinary game/class allocation and ownership after custom allocator removal remain pending. |
| `0013-check-native-startup-arena-and-sdk-heaps.patch` | Validate capacity and allocation/SDK heap results before using memory; retain original reserves and setup order. | Real original startup with 64/128 MiB MEM2, SDK allocate/free checks, repeated cleanup, synthetic Wii data, and owned USA revision 1 RVZ. |
| `0014-share-native-dvd-types-and-file-metadata.patch` | Forward native DVD declarations to Aurora, adapt file metadata/status access, and share canonical 64-bit SDK scalars. | Compiled original NL file code against real Aurora DVD; synthetic Wii file reads. |
| `0015-extract-original-graphics-prestartup.patch` | Select the original `glplatPreStartup()` helper without compiling the later GX/VI setup in the same file. | Original `nlInit()` advances past this unchanged helper, which returns true at the selected pin. No game graphics claim. |
| `0016-extract-original-basic-file-operations.patch` | Select original file wrappers and synchronous whole-file loading independently of unfinished advanced async/decompression. | Original open/read/close and whole-file tests; no successful replacements for unselected APIs. |
| `0017-adapt-native-disc-read-lifecycle.patch` | Keep callback context pointer-sized; validate read bounds/capacity and both head/tail slots; drain workers before callbacks/pool reuse; preserve reentrant callback state and clean file/allocator shutdown. | Known synthetic bytes, exact/padded/unaligned reads, seek/EOF, reentrancy, callback exceptions, active cancellation, file/request pool exhaustion, whole-file ownership/OOM/read failure, repeated cleanup. |
| `0018-share-native-sdk-compiler-macros.patch` | Share native compiler attribute/address macros across Wii, Aurora, and host headers. | Native SDK/NL/DVD compilation without conflicting macro definitions. |
| `0019-select-native-sanim-rotation-decoders.patch` | Keep console GQR/paired-single assembly in the console branch and select the port's scalar native rotations; document packed-byte/host-order contracts. | Exhaustive 16/12/8-bit rotation components, unsigned scales, byte weights, exact float results, guards and repeated setup; original core startup completes. Animation assets/playback remain pending. |
| `0020-connect-native-whole-file-load-lifecycle.patch` | Connect the native whole-file adapter to read-error/shutdown cleanup and document its opaque handles, ownership and worker-draining cancellation. | Synthetic async loads, buffers, failures, reentrancy/cancel/shutdown and targeted sanitizers; owned boot INI byte comparisons. Original INI parsing, compressed loads, bundles and caches remain pending. |
| `0021-preserve-native-container-allocation-ownership.patch` | Destroy game-allocated container nodes/trees before owning-arena free; reclaim failed aligned-new constructions; select native stack allocation for AVL. | Native pointers, partial construction, node exhaustion, tree release and sanitizer checks; host global new/delete remain standard. |
| `0022-select-checked-native-static-inventory.patch` | Select original layered model/texture lookup and insertion; add checked level/duplicate handling and repeatable partial cleanup. | Nested rollback/shadowing, file ownership, original AVL, native ball inventory and shutdown. Animation/skin/chunk methods remain unlinked. |
| `0023-select-native-static-texture-manager-lifecycle.patch` | Select static texture management with checked queue/index ownership, atomic node/index registration, stale-binding resolution and ordered teardown. | Exhaustion/recycling, node OOM, nested textures, tiled/palette bytes and actual CMPR diffuse rendering. Animated textures and original texture GPU methods remain unselected. |
| `0024-check-native-graphics-pools-and-frame-lifecycle.patch` | Use pointer-sized addresses/marks, align both frame halves, validate offsets before publication, restore selected allocators and reclaim failed/finished MEM1/MEM2 pools. | Pointers above 4 GiB, independent frame payloads, invalid/foreign markers, repeated original budgets, real GX invalidation and arena recovery. |
| `0025-select-original-preinitfs-memory-callback.patch` | Select the original graphics memory callback and requirements independently of the incomplete main; report native failure explicitly. | Native scene executes its exact original frame/resource budgets and texture capacity; repeated CPU initialization/shutdown and full-entry object compilation. This does not execute complete `glStartup`. |
| `0026-preserve-native-matrix-handle-chain.patch` | Widen native matrix addresses through allocation, model/global state, callers and render caches; select original matrix entry units with bounds and lifetime checks. | Above-4-GiB model/state propagation, active frame/resource extents, invalid and expired handles, OOM, repeated shutdown and actual scene camera/model matrices. Broader patched callers remain unselected. |
| `0027-adapt-native-packed-render-state.patch` | Keep raster/texture IDs 32-bit, replace texture-state assembly with Wii word-preserving arithmetic, and add explicit native lifecycle and argument checks. | Independent two-word oracle across every texture field, raster fields/defaults, state save/restore and a restricted real GX preview profile. Full view/sort dispatch remains pending; selected materials are enabled by later patches. |
| `0028-adapt-native-graphics-math.patch` | Compile original NL math and portable SDK projection/inverse routines with scalar native assembly replacements, defined angle wrapping and real Aurora quaternion calls. | Independent transform/alias/inverse/camera/projection fixtures and actual scene rendering. Host root seeds retain Newton refinements; Wii `frsqrte` and gameplay parity remain pending. |
| `0029-select-native-material-registry-and-alpha.patch` | Keep descriptor widths fixed, validate native registration, add teardown and extract original texture alpha preparation with checked inventory lookup. | Registry/restart, native parameter layouts, original alpha/depth/culling choices and resource rollback. Generic parameter setters remain unselected. |
| `0030-connect-static-material-programs-to-aurora.patch` | Adapt four original material/render TUs to bounded native vertex arrays, Aurora FIFO calls and an explicit unlit preview context; retain original TEV recipes and safe quantized scrolling. | Twelve Vulkan pixel cases, owned static ball material and scrolling profile. Stadium lighting/shadows, other material programs and full scene dispatch remain pending. |
| `0031-select-native-object-lighting-and-shadow-lookup.patch` | Select original object-light and static shadow calculations with verified missing constants, native matrix cache, bounded CI8/RGB5A3 lookup ownership and explicit palette byte order. | CPU input/tile/palette/filter/rollback checks and real Vulkan diffuse/specular/shadow results. Stadium/character/effect selection, skinned paths and dynamic shadow casters remain pending. |
| `0032-enable-original-material-lighting-and-shadows.patch` | Enable the selected original materials' vertex/doubled/ramp lighting and projected-shadow branches, retaining per-material flags and scoped native view inputs. | Pixel comparisons for lighting/ramp/shadow modes, changed matrices and state restoration; synthetic disc shadow loading. Full original view/task dispatch remains pending. |
| `0033-adapt-native-view-graph-and-packet-sorting.patch` | Keep original view traversal, layer/packet sorting and callback flags with complete native addresses, defined depth conversion, checked graph ownership and frame lifetimes. Select original projection/count helpers and slot pools. | Five sort modes, callback transitions, graph depth/cycles, expired frames, allocator recovery, sanitizer checks and real Vulkan view submission. |
| `0034-check-native-render-target-ownership.patch` | Preserve original target naming/registry with fixed-width descriptions, generation-checked handles, rollback and ordered shutdown. Native adapters own target pools and Aurora copy-texture state. | Target reuse, texture-index exhaustion, all five copy formats, fresh targets after GPU cache eviction and repeated teardown. Full game view layers, dynamic shadow geometry and task startup remain pending. |
| `0035-enable-native-shadow-material-and-mesh-writer.patch` | Preserve the original shadow material through bounded Aurora arrays/FIFO calls and explicit colour input. Replace the mesh builder's Wii field offset with a typed member; check frame mesh counts, writes, allocation failure and lifetime. | CPU bounds/ownership and sanitizer checks; real Vulkan volume accumulation, cancellation, depth occlusion and original mask blending using generated geometry. |
| `0036-select-original-shadow-layers-and-volume-pass.patch` | Select original cameras, eleven shadow partitions, update scheduling and light-camera setup; connect the original volume attachment and blend pass with checked native ownership. | Atlas layout, update intervals, failure rollback, target reuse, half-size copies and pixel coverage. Original character/stadium geometry, full scene selection and task startup remain pending. |
| `0037-select-native-model-copies-and-stadium-shadows.patch` | Select original model duplication with typed material sizes and bounded native records, retaining shared streams and independent packet/parameter storage. Extract original stadium shadow initialization/submission with explicit inputs. | Clone isolation, failure cleanup, indexed Vulkan shadow pixels and owned Vice geometry read from a checked compressed world bundle. Full world object loading and posed characters remain pending. |
| `0038-adapt-native-event-ownership-and-lifetimes.patch` | Preserve full owner addresses and explicit flag masks; retain original registry/listener algorithms with checked initialization, callback transfer, deferred removal and scoped connection lifetimes. Separate the three-argument event template from Wii controller headers. | Dynamic/static/no-data/three-argument delivery, callback mutation/exceptions, allocation failure, 4,096 grouped listeners, repeated arena recovery and targeted sanitizers. Queued dispatch follows in 0039–0040. |
| `0039-adapt-native-queued-event-lifetimes.patch` | Retain queued callback order and batch modes with checked counters, insertion and pool teardown. Cancel payloads before event destruction and retain caller ownership on rejected queues. | Delivery/cancellation, both destruction orders, callback/disposer exceptions, OOM, reentrancy, capacity and complete arena recovery. Full gameplay event ordering remains unverified. |
| `0040-select-original-event-dispatch-task.patch` | Extract the existing default task transition hook from Team.cpp; select original dispatch task delivery with checked allocation, reset and separate final teardown. | Original Run/reset, reuse after reset, typed task destruction and startup against synthetic and owned Wii data. Complete task manager/frame/movie integration remains unselected. |
| `0041-enable-native-specular-detail-blend.patch` | Select the original static detail/specular material and specular-light routines using bounded native arrays and typed FIFO calls. | Four independent texture/UV bindings, blend and lighting pixels, projected shadows, failure cleanup and owned static world models. Other stadium materials and complete world loading remain pending. |
| `0042-select-native-task-scheduling.patch` | Select original task scheduling with checked borrowed ownership, exception teardown and explicit movie service boundaries. Preserve original priority/state/ticker behavior. | Registration order, masks, transitions, per-task clocks, dilation, lifetime and allocation failures, real scheduled event dispatch, inactive movies and rejected active playback. Complete frame tasks and movie decoding/audio remain pending. |
| `0043-select-original-graphics-frame-lifecycle.patch` | Extract original begin/end/send/discard state with checked host frame ownership, drained memory reuse and failure cleanup. | Original state/counter order, repeated discards, scheduler-driven calls, Vulkan pixels, partial-render recovery and arena teardown. Full graphics startup and game frame tasks remain pending. |
| `0044-adapt-native-configuration-and-strings.patch` | Retain original configuration parsing with native array/string ownership, checked capacities, copy-on-write ranges and owned asynchronous loads. | Typed values, failed replacements, cancellation/reentrancy, global lifetime, owned boot configuration and arena recovery. |
| `0045-enable-native-scrolling-specular.patch` | Select the original two-texture scrolling specular material with native arrays, explicit preview time/view inputs and refreshed specular lights. | Signed UV conversion, clamp/repeat scrolling, lighting modes, highlights, projected shadows, alpha and cleanup pixels, plus owned Vice/Crater Field models. Complete world loading and task/frame integration remain pending. |
| `0046-enable-native-camera-scrolled-overlay.patch` | Select the original position-generated overlay, lighting and shadow recipe with bounded native arrays and an explicit active-camera input. Reject non-finite texture matrices and reset registry state. | Camera movement, scale, three bindings, alpha/mask pixels, original wrap mutation, failure recovery and owned stadium models. Preview keeps authored coordinate scale; the original camera manager and complete world loading remain unselected. |
| `0047-adapt-native-tweak-registration.patch` | Size native registry pools, preserve borrowed values and registration order, select existing value methods and bound parsing. | Pre-memory registration, typed values, repeated initialization, native bindings, static shutdown, and arena recovery. Dynamic state push/reset remains pending. |
| `0048-select-original-frame-timing.patch` | Select original phase timing, history and histograms independently of debug drawing and gameplay predicates. | Original scheduler, tick rollover, 30-frame averages, both history wraps, conditional samples and arena recovery. |

The initial explicit game allocator is adapted; complete game allocation,
math, pointer-bearing interfaces, data conversion, and Wii services remain
work in progress. The
[experimental startup](../docs/RUNTIME.md#experimental-original-startup) links a
bounded original prefix with explicit missing-service diagnostics.

Only `include/`, `libs/`, and `src/` are exported from the committed decomp
snapshot. Patches adding other top-level paths are rejected. The full pristine
submodule remains available for source reference, notices, and decomp metadata.

## nod series

`nod/0001-lock-cargo-dependencies.patch` makes Corrosion pass `--locked` to
Cargo metadata and build commands. The pinned upstream `Cargo.lock` is retained
unchanged. The nod and Corrosion submodules are prepared and verified using the
same process as the decompilation.

## Aurora series

`aurora/0001-isolate-core-build-dependencies.patch` makes SQLite conditional on
GX, matching where Aurora defines that dependency, and stops Aurora's dependency
setup from forcing the parent `BUILD_TESTING` cache entry off. The optional
Aurora host check exercises the build with GX disabled and keeps the port's
own tests enabled. It does not add replacements for missing game functions.

`0002-guard-optional-rmlui-header.patch` guards the RmlUi include in the window
code, which otherwise imports WebGPU headers with both GX and RmlUi disabled.
`0003-declare-core-vi-link-dependency.patch` records core's use of VI framebuffer
sizing, allowing CMake to resolve the mutual static-library dependencies.

`0004-implement-mem2-and-os-memory-lifecycle.patch` connects Aurora's existing
MEM2 size configuration to real owned memory and adds checked Wii MEM2 arena
calls. Shutdown resets SDK heap descriptors, arena pointers, and OS initialization
before freeing MEM1/MEM2. Core's OS dependency is explicit. Linux startup checks
cover 64/128 MiB MEM2 and repeated initialization; Windows debug reservations
have a release path but remain unverified. Wii address translation and IOS
memory-map parity are not implemented by this patch.

`0005-respect-graphics-providers-and-enable-validation.patch` uses the parent's
prepared FreeType, SQLite, and Zstandard targets instead of looking up system
substitutes. A default-off configuration field enables Dawn backend validation
for the separate GX diagnostic. The separate core/startup presets retain their
existing null backend with GX disabled.

`0006-keep-validation-for-release-diagnostics.patch` retains WebGPU API
validation and robustness when backend validation is requested, including in
Release builds. Other callers retain Aurora's existing optimization flags.
`0007-release-gx-default-resources-on-shutdown.patch` releases the default GX
bind group, sampler, texture/view, and pipeline layout before device/window
shutdown. Retained static references caused a Vulkan validation-layer crash
at process exit in the initial desktop check.

`0008-finish-frames-without-presentation.patch` closes discarded or failed host
frames without acquiring or presenting a surface. Queued work and completion
callbacks still finish; this does not roll back writes to the emulated framebuffer.
The next successful frame redraws it. Existing callers retain normal presentation.

## Dawn series

The series targets Dawn `1155e0ed531126f33a1279afa029349651ca1c93`.
`0001-keep-generator-bytecode-out-of-prepared-source.patch` passes Python `-B`
to Dawn's JSON generators, including their configuration-time invocations.
`0002-keep-spirv-generator-bytecode-out-of-source.patch` applies the same rule to
the pinned SPIRV-Tools source generators. Their generated C++/headers/tables
remain under the binary directory; imported helpers and Jinja/MarkupSafe do not
leave bytecode beside the immutable prepared source.

`0003-accept-explicit-exported-source-revision.patch` exposes the version
generator's existing revision-file argument to CMake. The parent provides
the verified Dawn base pin; without it the exported tree discovers the port's
Git HEAD. The generated Dawn version therefore identifies Dawn's actual source.

`0004-respect-disabled-vulkan-robustness.patch` adds per-pipeline Vulkan
robustness settings only when device robustness is enabled. Dawn previously
added those settings even when its optimization toggle disabled the required
device feature, producing validation errors on the tested Intel Mesa driver.

The graphics build's selected nested gitlinks must also be supplied when
manually preparing or exporting a Dawn patch. Use the paths listed in
`cmake/Dawn.cmake` as repeated `--nested-submodule PATH` arguments. Omitting the
selection requests all of Dawn's recorded nested sources and changes the inputs.

## Develop a new patch

The examples below use `build/`. Use `build/release` or `build/debug` instead
when working with a preset, and add `--dependency nod` for nod's patch series.

1. Configure once to create a clean prepared tree.
2. Edit the relevant files under `build/prepared/mscharged-decomp/source/`.
3. Export those edits relative to the currently applied series:

   ```sh
   python3 tools/prepare_sources.py --build-dir build --export-patch build/new-change.patch
   ```

4. Review the diff, give it a focused name under `patches/mscharged-decomp/`,
   add a `Subject`, purpose, and `Upstream-base` explanation before the diff,
   then append its filename to `series`. Keep independent new port code in
   this repository's `src/` where possible.
5. Once the edits have been preserved in the patch, regenerate explicitly:

   ```sh
   python3 tools/prepare_sources.py --build-dir build --discard-generated
   cmake -S . -B build
   cmake --build build
   ctest --test-dir build --output-on-failure
   ```

The tool refuses to overwrite edited generated source without
`--discard-generated`. Export before changing the pin, patch series, or
preparation tool, so the comparison still has the correct baseline. Exported
patches are additional changes on top of the current series. They can include
new files and deletions; inspect both carefully.

Do not edit the manifest to approve local changes. If a preparation process is
interrupted and leaves an empty `build/prepared/.<dependency>.lock` directory,
check that the process has stopped before removing that lock and retrying.

## Update a dependency

Select an explicit published commit, preferably a decomp release once one is
available. Update and stage the submodule pin, review upstream differences,
update `base`, and refresh or retire the affected patches. Reconfigure, compile,
and validate the affected behavior as one reviewable change. Preserve previous
pins and patch sets in Git history.

Until the first 100% decompilation and 100% link release is tagged, development
uses an explicitly selected incomplete snapshot. A newer snapshot may contain
unfinished or changed code. Keep the enabled native source subset explicit;
patch application and successful utility tests do not establish a complete
game link or gameplay parity.

An annotated tag can name an incomplete development baseline. In the decomp
repository, tag the exact intended commit and publish that tag:

```sh
git tag -a port-baseline-2026-10-01 <full-commit-sha> -m "Incomplete decomp snapshot for native port development"
git push origin refs/tags/port-baseline-2026-10-01
```

Use `HEAD` in place of `<full-commit-sha>` to tag the current committed state;
uncommitted edits are not included. Reserve `1.0` for the planned complete
decompilation/link release. Keep published tags fixed and use new tags for later
snapshots. A tag is a readable label; the port's gitlink and this series' `base`
still record the exact commit. Creating or pushing a tag does not change this
port's pin. Record its label when deliberately adopting the corresponding commit.
