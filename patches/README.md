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

## Initial Charged series

| Patch | Reason | Current validation |
| --- | --- | --- |
| `0001-use-standard-fabs-in-nlMath-header.patch` | The shared math header uses CodeWarrior's undeclared `__fabs`; native Clang rejects `nlRandom.cpp`. Use the standard float overload. | Native compilation; finite, signed-zero, infinity, and NaN checks. |
| `0002-keep-mt-seed-state-32-bit.patch` | Wii `unsigned long` is 32 bits; Linux/macOS LP64 makes it 64 bits. Keep the available MT seed state and public argument explicitly 32-bit. | Signature/type checks and compilation of the available seed implementation. No complete MT generator is present in this subset. |
| `0003-fix-native-rvl-scalar-types.patch` | Keep SDK 32-bit scalars/calendar fields fixed-width; use standard pointer types and avoid host type conflicts. | Startup compilation and scalar/calendar layout assertions. |
| `0004-preserve-native-chunk-addresses.patch` | Align and traverse chunk pointers without truncating host addresses; retain 32-bit serialized size fields. | Native allocation/alignment/traversal checks, including addresses above 4 GiB when supplied by the host. Bounds and endian conversion remain separate work. |
| `0005-include-replay-pose-node-definitions.patch` | Template bodies need complete replay pose types when parsed by host compilers. | Compilation of the original game entry unit. |
| `0006-expose-original-startup-core.patch` | Extract the original region/language/`nlInit()` prefix so it can run before the rest of the game links. Skip PowerPC-only GQR assembly on native builds. | Original USA language branches and real `nlInitMemory()` entry. Native animation decoding remains pending. |
| `0007-keep-native-arena-addresses.patch` | Compute arena capacity from pointers without first truncating their addresses. | Real MEM1/MEM2 startup arena sizes; broader Wii address translation remains pending. |
| `0008-use-standard-memory-header.patch` | Replace four MSL `mem.h` include sites with the host `string.h`. | Native headers and entry compilation; no synthetic `mem.h` shim. |
| `0009-read-native-bus-clock-through-host.patch` | Native timer units obtain the initialized Aurora bus clock through a host adapter. | Compilation/linking of original ticker/time units alongside Aurora. Original game scheduling remains pending. |
| `0010-use-host-placement-new.patch` | Use standard placement new and native allocation-operator argument widths. | Startup, allocator, and original entry compilation. Full game/class-specific allocation remains pending. |
| `0011-adapt-original-free-list-allocator.patch` | Retain the original free-list algorithm with native pointer arithmetic, metadata size/alignment, and explicit invalid-request/OOM errors. | Mixed allocations from both ends, alignment, exhaustion, payload preservation, complete coalescing, and standalone ASan/UBSan checks. |
| `0012-route-native-game-frees-by-arena-ownership.patch` | Route explicit game frees by owning arena rather than console address bits; keep host global new/delete standard. | MEM1/MEM2 allocations freed while a different arena is selected. Ordinary game/class allocation and ownership after custom allocator removal remain pending. |
| `0013-check-native-startup-arena-and-sdk-heaps.patch` | Validate capacity and allocation/SDK heap results before using memory; retain original reserves and setup order. | Real original startup with 64/128 MiB MEM2, SDK allocate/free checks, repeated cleanup, synthetic Wii data, and owned USA revision 1 RVZ. |

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
