# GitHub builds

`main` contains development work. `stable` holds reviewed snapshots, promoted
through a `main` → `stable` pull request. A stable snapshot is still a work in
progress until the game is complete.

The **Build binaries** workflow checks PRs targeting `stable`. After a merge
into `stable`, successful Linux x86_64 and macOS Apple Silicon jobs upload
archives under **Actions → Build binaries → run → Artifacts**, retained for
14 days. Manual runs validate the selected branch; only `stable` publishes
binary archives. Failed checks may attach configuration logs. These are unsigned
development builds, not tagged GitHub Releases.

Windows runs host service tests with a pinned LLVM-MinGW compiler. Its full
module linking, pointer ABI and file storage still need integration before it
can produce game binaries. The README build badge reports the `stable` workflow;
it covers Linux/macOS builds and the Windows host tests. A successful
compile does not establish gameplay or hardware compatibility.

Extract the downloaded `.tar.gz`, keep both runtime modules and `assets/`
beside the executable, and run with your own disc:

```sh
./mscharged --disc /path/to/R4QE01.rvz --window
```

Game data, personal settings and saves are excluded. Linux builds use Ubuntu
26.04/GCC 15 and need compatible system libraries plus a Vulkan driver. macOS
builds use macOS 15/Apple Clang and need Metal. These archives are not
self-contained installers.

## Initial repository setup

Rename the old `master` branch to `stable` using GitHub's branch settings.
The workflow and this policy enter that branch with the first reviewed PR;
merely renaming the old branch does not promote current development code.
Protect `stable` against direct pushes and require PR review and the Linux/macOS
build checks. Add Windows as a required check once its runtime is supported.
