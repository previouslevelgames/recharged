# GitHub builds

`main` contains development work. `stable` holds reviewed snapshots, promoted
through a `main` → `stable` pull request.

The **Build binaries** workflow builds Linux x86_64, Windows x86_64 and macOS
Apple Silicon packages for PRs targeting `stable` and for pushes to `stable`.
Packages appear under **Actions → Build binaries → run → Artifacts**: a PR's
for 7 days (to test before merging), `stable`'s for 14 days. Failed checks may
attach configuration logs. Releases are drafted from version tags (below);
all builds are unsigned. A separate job compiles the Windows host-service
tests natively.

Windows packages are cross-compiled on Ubuntu with the pinned LLVM-MinGW
toolchain and include its C++ runtime DLLs (`libc++`, `libunwind`,
`winpthread`) with their notices.

Extract the downloaded `.tar.gz` (Linux, macOS) or `.zip` (Windows), keep the
runtime modules, DLLs and `assets/` beside the executable, and start the
launcher (`mscharged`, or `mscharged.exe`), or run the game directly with your
own disc:

```sh
./mscharged --disc /path/to/R4QE01.rvz --window
```

Game data, personal settings and saves are excluded. Linux builds use Ubuntu
26.04/GCC 15 and need compatible system libraries plus a Vulkan driver. Windows
builds need Windows 10/11 and a Vulkan driver. macOS builds use macOS 15/Apple
Clang and need Metal. These archives are not self-contained installers.

## Releases and version numbers

The version is the `project(mscharged_port VERSION x.y.z ...)` line in
`CMakeLists.txt` ([Semantic Versioning](https://semver.org)): a **patch** for
bug fixes (1.0.0 → 1.0.1), a **minor** for new features (1.0.1 → 1.1.0), a
**major** for breaking changes. To release:

1. On `main`, raise the version and commit it with the changes it covers:

   ```sh
   python3 tools/bump_version.py patch   # or minor, major, x.y.z
   ```

2. Open the `main` → `stable` pull request; its run builds and uploads the
   packages for testing. Merge it.
3. Tag the merge commit on `stable` and push the tag:

   ```sh
   git fetch origin && git tag v1.0.1 origin/stable && git push origin v1.0.1
   ```

4. The tag's run checks that the tag matches the project version, builds the
   three packages and drafts a GitHub release with them, `SHA256SUMS` and
   notes generated from the changes since the last tag. Review the draft,
   add a few words and publish it; the README links to the latest release.

## Initial repository setup

Rename the old `master` branch to `stable` using GitHub's branch settings.
The workflow and this policy enter that branch with the first reviewed PR;
merely renaming the old branch does not promote current development code.
Protect `stable` against direct pushes and require PR review and the Linux,
Windows and macOS build checks.
