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

The current base is `d6850f8dae162827f94d3f9b01a91538c02849dd`, adopted from
published upstream main. It remains an incomplete development snapshot. This
update includes matching, source-linked pose accumulation, world visibility and
DebugWriteCache. Patches 0038 and 0066 follow revised event/header context;
0084 preserves the matched pose arithmetic, adapts the two PowerPC return-value
captures and retires the native world-matrix initialization workaround now
covered upstream. Native source selection and validation remain explicit.
The audio backend, bank loader and bundle manager are now source-linked;
AudioSource remains incomplete. Patch 0094 follows its updated header and field
names. Patches 0098–0099 share original resident source states and frontend
pointer production for checked native owners; full audio and menu startup remain
in development.

Patch 0111 shares the inherited empty `InitializeSubHandlers` stage used by
Main and Options. Their selected native visual owners share one base handler
with the scene stack, preserving one original update before input. Complete
scene creation and game services remain separate integration work.

Patch 0113 shares original option defaults and selected save-operation flag
rules. The native preferences provider uses these rules with a dedicated host
file; it does not load or complete the original Wii game save.

Patch 0112 shares the original Audio Options controls and category volume rules.
The selected native screen can change real resident and streamed audio gains;
complete audio startup, navigation and game-save integration remain separate.

Patch 0114 shares the original Visual Options controls, pointer feedback and
zoom settings. The selected native screen preserves the original Back behavior
and can save scoped native preferences; gameplay camera and full game saves
remain separate services.

Patch 0115 shares Main's original Options action. The native adapter queues the
actual source scene pop, hides navigation, plays its cue and starts the original
transition script. Missing transition services remain explicit.

Patch 0116 shares Audio Options' original save feedback while leaving the Wii
save call intact. Its selected native owner now uses the scene stack's single
base update and original input lock, with separately scoped preference saving.

The selected camera sources include original authored playback (0056) and
desktop DebugCam controls (0057). Patch 0058 shares the original frontend
camera catalog between the console factory and native loading code.
The ordered patches are listed in [`mscharged-decomp/series`](mscharged-decomp/series).

Upstream now marks both `nlEvent.cpp` and `MemAlloc.cpp` matching. Patch 0038
connects the original immediate event registry and listeners to native startup
checks. Patches 0039–0040 add queued payload cleanup and the original dispatch
task; the complete game task loop remains pending. Matching Wii code and a
strictly applicable patch series do not establish a complete native game;
see [the entry and runtime scope](../docs/RUNTIME.md#experimental-original-startup).

Patch 0118 shares the original Audio/Visual Done hover callbacks and fixed
button bounds. Native submenu input uses the retained NAV components and
explicit host preference persistence; full Wii save and haptic services remain
separate work.


Patch 0120 shares the original Credits phase, text and scrolling rules. Its
selected native owner reaches the NLG movie request; actual playback and
completion remain explicit integration work.

Patch 0123 shares original movie decode cadence, PCM mixing and float-quad
construction. The selected native provider reads through NL, outputs through
SDL, and renders with the original YUV movie material. Completion requires the
exact final frame's presentation and flushed audio consumption. Full game audio
mixing and live volume changes remain separate work.

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
| `0031-select-native-object-lighting-and-shadow-lookup.patch` | Select original object-light and static shadow calculations with source-defined constants/defaults, native matrix cache, bounded CI8/RGB5A3 lookup ownership and explicit palette byte order. | CPU input/tile/palette/filter/rollback checks and real Vulkan diffuse/specular/shadow results. Stadium/character/effect selection, skinned paths and dynamic shadow casters remain pending. |
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
| `0047-adapt-native-tweak-registration.patch` | Size native registry pools, preserve borrowed values and registration order, adapt shared template allocation/parsing, and use actual typed binding storage. | Pre-memory bool/int/float registration, checked parsing, repeated initialization, borrowed bindings, sanitizer checks and arena recovery. Dynamic state push/reset remains pending. |
| `0048-select-original-frame-timing.patch` | Select original phase timing, history and histograms independently of debug drawing and gameplay predicates. | Original scheduler, tick rollover, 30-frame averages, both history wraps, conditional samples and arena recovery. |
| `0049-enable-native-masked-detail-blend.patch` | Select original mask-weighted detail blending, optional light-ramp binding, lighting and projected shadows with bounded native vertex arrays. | Three independent bindings/UV sets, per-channel masks, blend endpoints, lighting, alpha, rollback and owned Crater Field geometry. |
| `0050-enable-native-scrolling-masked-detail-blend.patch` | Retain the original detail blend and independently animate all three texture coordinates using the explicit preview time. | Six signed scroll speeds, clamp/repeat, lighting, shadows and cleanup pixels. Owned Palace/Bowser instances render with animated `_ifl` textures after patch 0054. |
| `0051-enable-native-scrolling-camera-overlay.patch` | Select the original UV1 diffuse input, position-generated overlay and address-based mask-scroll behavior with explicit time/camera inputs. | Camera/UV independence, signed scrolling, wrap mutation, lighting, alpha, shadows and failure recovery. Zero scale rejects; authored camera integration remains pending. |
| `0052-select-native-camera-core.patch` | Select original camera stack, transition and pose routines with native allocation, borrowed ownership, checked callbacks and repeatable teardown. | Stack order, interpolation, callback failures, allocation recovery and supplied preview poses. Authored cameras, factory/impostor services and full frame tasks remain pending. |
| `0053-preserve-native-camera-filter-arithmetic.patch` | Preserve Wii noise hashing through explicit 32-bit wrapping, initialize inactive displacement and validate native filter timing and inputs. | Independent noise values, real task-state rumble gating, clamped steps, reset/expiry and sanitizer checks. Wii presentation parity remains pending. |
| `0054-select-native-texture-animation.patch` | Select original IFL playback and inventory traversal with bounded native records, owned animation indices and ordered release. Material binding and alpha preparation resolve the current frame. | Fixed-width decoding and dependency checks, loop/ping-pong/hold/pause timing, alias refresh, allocation failure, rollback, sanitizer/leak checks, Vulkan pixels and owned Palace/Bowser models. Frames must be static textures in the same decoded batch; full world/task integration remains pending. |
| `0055-select-native-camera-data.patch` | Select original camera-data initialization separately from playback; initialize ownership and pair game-allocated arrays with game frees. | Bounded big-endian CAM decoding, full names, native handles, allocation rollback and sync/async NL file lifetimes. |
| `0056-select-native-animated-camera-playback.patch` | Select original camera interpolation, cuts, transforms, timing and focal calculations; provide explicit display/simulation inputs and validate sampling. | Retained asset ownership, loop/end callbacks, mirroring, facing, FOV, original CameraMan updates and authored scene preview. Full camera factory and DOF rendering remain pending. |
| `0057-select-native-debug-camera.patch` | Select original desktop orbit/pan/distance/height controls and pose math through explicit native input values; define angle wrapping and reject degenerate look-at inputs. | CameraMan borrowing, SDL input mapping, static preview, failure cleanup and selected sanitizers. Focused input-to-render qualification, Wii DPD, player/replay targets and full factory selection remain pending. |
| `0058-share-original-frontend-camera-catalog.patch` | Share the unchanged original filename/alias table and record type; retain the console factory's ordering and request loop. | Strict preparation and exact ordered comparison with the pinned source; native loading uses the same 37 entries. Full frontend factory and NIS selection remain separate. |
| `0059-select-native-interpreter-execution.patch` | Select original interpreter execution and operations with checked native stacks, frame/string references and typed host services. | All original opcodes, available operations, calls/returns, pause/retry, budgets, malformed inputs and selected sanitizer checks. Real NIS trigger collection and game services are separate integration steps. |
| `0060-share-original-nis-playback-timing.patch` | Share original NIS frame clamp/carry and trigger crossing arithmetic with bounded camera scheduling. | Camera timing, ordered trigger dispatch, explicit missing services and selection/teardown tests; complete NisPlayer actor and effect services remain pending. |
| `0061-select-original-nis-trigger-definitions.patch` | Select original trigger-definition service bodies and name hashing with checked native arguments and owned records. | All 310 owned functions and 457 name selections match an independent bytecode oracle; generated and sanitizer checks pass. Effect/audio/event execution remains separate. |
| `0062-share-original-graphics-startup-stages.patch` | Share original memory/state/view startup stages and frame task priorities. Native graphics own the selected material profile and host services transactionally. | Original scheduler order, rollback, arena recovery, synthetic Vulkan and owned frontend rendering pass. Full glStartup, remaining programs and game tasks stay unselected. |
| `0063-select-native-frontend-input.patch` | Select original FE input/repeat/focus and pad delegation with native ownership, defined polar wrapping and the shared action remap. | Four-pad desktop input, focus/capture/hotplug gates, SDL sampling and selected sanitizers pass. Wii gameplay motion and rumble remain separate. |
| `0064-hide-unselected-console-pad-class.patch` | Hide the unused concrete console pad class in native builds so speculative devirtualization cannot introduce its console allocator. | Optimized original input tests and the native scene executable link pass; console source behavior is unchanged. |
| `0065-select-native-hierarchy.patch` | Select original hierarchy accessors and traversal while disabling the unsafe Wii in-place loader. A checked reader owns native pointer tables and translation offsets separately. | Generated format/traversal oracles, malformed inputs, allocation rollback, retained lifetimes, eight owned frontend rigs and selected sanitizers pass. Pose sampling and skinning remain separate. |
| `0066-share-original-async-loading-steps.patch` | Share original loading sequence, yield/readiness equations and persistent-pool inputs with a bounded native boot owner. Select only source-defined empty debug markers. | Synthetic sequence, yield, limits and pool-lifetime checks; owned boot bytecode stops explicitly at particle loading. The complete loading dispatcher and frontend remain unlinked. |
| `0067-select-native-sanim-sampling.patch` | Select original root, weight and equal-count morph sampling on retained native animation records. Disable Wii in-place loading and unselected callback ownership; bound sampling and avoid reading past constant/endpoint weight keys. | Checked channels, independent generated sampling comparisons and owned frontend animations. Unequal-count morph sampling, pose accumulation and skinning remain unqualified. |
| `0068-share-original-nis-pip-steps.patch` | Share original PIP rectangles, expansion/swap timing and target settings with retained native cameras and rendering. | Exact endpoint and swap tests, target ownership, synthetic-disc entry and two-camera Vulkan composite pixels. Full NisPlayer actors, audio and Holotron remain separate. |

Patch `0072-select-native-effects-registration.patch` shares original group/template
resolution, persistence and cleanup on retained native records. Exact authored
colour counts and resource IDs are preserved; user-effect factories, geometry
registration and particle simulation remain separate services.

Patch `0073-share-original-frontend-animation-steps.patch` shares original key
sampling, Bezier interpolation and presentation/slide timing with a checked
native owner. Authored animation runs in the scene preview; full frontend scene
handlers and menu transitions remain unlinked.

Patch `0074-select-native-particle-simulation.patch` selects original particle
emission, motion, RNG and quad sampling for a checked CPU owner. It retains the
authored colour table, bounds native sample conversions and pairs atlas memory
with its game allocator. GL registration, lighting and rendering stay unselected.

Patch `0075-share-original-frontend-selection.patch` shares the original
presentation/component clock reset predicates. The native scene session owns
actual FEN/font/image loading and retained frames; it does not supply the original
scene-manager stack, handler callbacks or menu readiness.

Patch `0076-select-original-particle-billboards.patch` shares original billboard
raster and quad writing, selects the textured-colour mesh writer and handles its
signed short UV streams on the native vertex-colour material path. Real texture
bindings and original frame storage are owned by the bounded renderer.

Patch `0077-share-original-frontend-instance-steps.patch` shares original named
instance traversal, setter effects and the loading scene's component setup.
Native mutations publish complete retained layouts; the HOME-menu manager and
full scene-handler lifecycle remain separate dependencies.

Patch `0078-include-tweak-pool-destruction.patch` exposes the original inline
slot-pool destructors to native tweak-registry exception cleanup. Optimized
builds must not depend on another translation unit emitting those definitions.

Patch `0079-share-original-font-text-steps.patch` shares original string metrics
and page-ordered glyph generation with bounded native font adapters. Plain
colour text retains fractional draw advances and original short UVs; formatted
effects and scissored text remain separate work.

Patch `0080-share-original-font-loading-steps.patch` shares the original texture
page order and completion predicate with staged native NL font reads. Decoded
font assets are published together; full FontManager graphics registration is
still a separate integration step.

Patch `0081-drain-failed-native-raw-reads.patch` removes failed raw NL requests
and paired tails before optional whole-file cleanup. Workers are joined,
unrelated reads survive, and the original I/O failure remains visible.

Patch `0082-select-native-font-polygons.patch` selects original textured `glPoly2`
packets and shares font raster setup. Native font pages retain real pool/index
bindings through frame completion; matrix state is restored on failed submission.
Formatted, effect and scissored text remain unselected.

Patch `0083-share-original-emission-controller-steps.patch` shares controller
timing, completion callbacks, stop guards and manager ID progression. Native
controller groups retain the original particle pool, atlas and RNG together;
pose, model, light, user-effect and replay services remain separate work.

Patch `0084-select-native-pose-accumulator.patch` selects matched original
pose blending and matrix construction with paired native array ownership,
allocation rollback and defined temporary lifetimes. The TU is now matching
and linked upstream. Independent CPU checks cover transforms and ownership;
pose-tree evaluation, replay and complete character rendering remain pending.

Patch `0085-select-original-frontend-image-packets.patch` selects original image
quad packets and shares their raster setup. Mixed frontend frames retain image
and font registrations through submission, cancellation and transactional reload.

Patch `0086-share-original-frontend-handler-steps.patch` shares base update,
activation, screen-ring and loading-notification rules. Native scene ownership
uses real frontend input; the original manager's state-6 gate and unavailable
concrete handlers and HOME services remain explicit boundaries.

Patch `0087-share-original-retail-boot-loading.patch` shares the retail boot
screen's setup, input, fade and phase selection with a retained native adapter.
It stops at the original logo sound request until audio services are available.

Patch `0088-share-original-sanim-pose-steps.patch` shares the original bone-channel
interpolation and unmapped-node fallback with native pose sampling. Native bounds
checks handle singleton and rounded terminal samples; complete animation
controllers, morphs and skinning remain separate work.

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

`nod/0002-join-preloader-workers-before-stream-release.patch` disconnects and
joins read-ahead workers before the final reader releases its stream. This keeps
FFI close callbacks inside the host I/O lifetime. Generated-disc tests cover an
active blocked read, retained partition/file readers, and threading-disabled
builds.

## Aurora series

Patch 0011 adds bounded video/PCM entry points to the existing THP decoder,
sharing its valid decoding arithmetic. Native movie reads validate container,
frame and output bounds. Decoding is separate from presentation and playback.

Patch 0012 records tagged GX draws after they are encoded into the render pass.
Native movie playback combines this evidence with successful frame presentation
and drain before accepting a displayed frame. A skipped cold pipeline does not
issue draw evidence; an encoded draw alone does not prove GPU completion.

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

`0009-report-successful-presentation-geometry.patch` records the exact content
rectangle and window identity only after successful surface presentation. Native
pointer routing uses that retained snapshot; discarded frames do not advance it.
The snapshot is empty with GX disabled and resets on initialization/shutdown.

`0010-retain-dvd-handles-on-allocation-failure.patch` closes opened nod/overlay
handles if command allocation fails and reports failure through the existing
DVD admission result. Native NL callers can then restore their reserved request
slot and pending count. Synthetic tests cover both providers and all 64 slots;
music replacement tests also exercise this path under scoped sanitizers.

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
