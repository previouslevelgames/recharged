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

The complete math implementation, allocator, pointer-bearing interfaces, data
conversion, and Wii runtime services are not adapted by these two patches.

## nod series

`nod/0001-lock-cargo-dependencies.patch` makes Corrosion pass `--locked` to
Cargo metadata and build commands. The pinned upstream `Cargo.lock` is retained
unchanged. The nod and Corrosion submodules are prepared and verified using the
same process as the decompilation.

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
