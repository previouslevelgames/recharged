#!/usr/bin/env python3
"""Initialize only the recorded source dependencies for the chosen build."""
from __future__ import annotations

import argparse
from pathlib import Path
import platform
import shlex
import subprocess
import sys
import time


LAUNCHER = (
    "extern/mscharged-decomp", "extern/nod", "extern/corrosion",
    "extern/sdl", "extern/imgui",
)
RUNTIME = LAUNCHER + (
    "extern/aurora", "extern/dawn", "extern/fmt", "extern/xxhash",
    "extern/tracy", "extern/zlib-ng", "extern/libpng", "extern/freetype",
    "extern/sqlite", "extern/zstd",
)
# The Nintendo Switch build: the game runtime without nod and SDL3, with
# sdl3on2 for SDL and mesa-switch for the NVK driver (tools/switch/build-nvk.sh).
SWITCH = (
    "extern/mscharged-decomp", "extern/aurora", "extern/dawn", "extern/fmt", "extern/xxhash",
    "extern/tracy", "extern/zlib-ng", "extern/libpng", "extern/freetype", "extern/sqlite",
    "extern/zstd", "extern/imgui", "extern/sdl3on2", "extern/mesa-switch",
)
DAWN_COMMON = (
    "third_party/abseil-cpp", "third_party/jinja2", "third_party/markupsafe",
    "third_party/spirv-headers/src",
    # Required by the shared Dawn preparation patches, including on Metal.
    "third_party/spirv-tools/src",
)
DAWN_VULKAN = (
    "third_party/vulkan-headers/src",
    "third_party/vulkan-utility-libraries/src",
)
# Seconds to wait before each new checkout attempt: a source host that is
# briefly unavailable (HTTP 503 from chromium.googlesource.com) should not fail
# a build. Checkouts that already reached their pin are kept between attempts.
RETRY_DELAYS = (10, 30, 60)


class SetupError(RuntimeError):
    pass


def git(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args], capture_output=True, text=True,
    )
    if result.returncode:
        raise SetupError(result.stderr.strip() or f"git {' '.join(args)} failed")
    return result.stdout


def plan(launcher: bool, system: str, switch: bool = False):
    if switch:
        return SWITCH, (("extern/dawn", DAWN_COMMON + DAWN_VULKAN), ("extern/freetype", ("subprojects/dlg",)))
    if launcher:
        return LAUNCHER, ()
    if system not in ("Linux", "Darwin"):
        raise SetupError("The current game runtime selects Linux/Vulkan or macOS/Metal. "
                         "Use --launcher for the smaller launcher build.")
    dawn = DAWN_COMMON + (DAWN_VULKAN if system == "Linux" else ())
    return RUNTIME, (("extern/dawn", dawn), ("extern/freetype", ("subprojects/dlg",)))


def pins(repo: Path, paths: tuple[str, ...]) -> dict[str, str]:
    # Source preparation also treats the stage-zero parent index as authoritative.
    result = {}
    for path in paths:
        rows = [row for row in git(repo, "ls-files", "--stage", "-z", "--", path).split("\0") if row]
        if len(rows) != 1:
            raise SetupError(f"{repo / path}: expected one recorded submodule pin")
        metadata, actual_path = rows[0].split("\t", 1)
        mode, commit, stage = metadata.split()
        if mode != "160000" or stage != "0" or actual_path != path:
            raise SetupError(f"{repo / path}: expected an unmerged-free submodule pin")
        result[path] = commit
    return result


def check_gitmodules(repo: Path, paths: tuple[str, ...]) -> None:
    changed = git(repo, "status", "--porcelain=v1", "--untracked-files=no", "--", ".gitmodules")
    if changed:
        raise SetupError(f"Tracked changes in {repo / '.gitmodules'}; preserve your edits before setup")
    declared = git(repo, "config", "--file", ".gitmodules", "--get-regexp", r"^submodule\..*\.path$")
    names = {row.split(maxsplit=1)[1] for row in declared.splitlines()}
    if not set(paths).issubset(names):
        raise SetupError(f"Required paths are missing from {repo / '.gitmodules'}")


def check_existing(repo: Path) -> None:
    if not repo.exists():
        return
    if repo.is_symlink() or not repo.is_dir():
        raise SetupError(f"Dependency path is not a regular directory: {repo}")
    if not (repo / ".git").exists():
        if any(repo.iterdir()):
            raise SetupError(f"Dependency directory contains local files without a Git checkout: {repo}")
        return
    if Path(git(repo, "rev-parse", "--show-toplevel").strip()).resolve() != repo.resolve():
        raise SetupError(f"Not an independent dependency checkout: {repo}")
    # Check the selected children separately. A clean child at the wrong commit
    # needs a pinned checkout, rather than being treated as a source edit.
    dirty = git(repo, "status", "--porcelain=v1", "--untracked-files=no", "--ignore-submodules=all")
    # Inside a dependency, staged gitlink edits are source edits too. Only the
    # port's own stage-zero index selects its direct dependency revisions.
    staged = git(repo, "diff", "--cached", "--name-only", "--ignore-submodules=none")
    if dirty or staged:
        raise SetupError(f"Tracked changes in {repo}; preserve your edits before setup")


def update_command(repo: Path, paths: tuple[str, ...], shallow: bool) -> list[str]:
    # Shallow: fetch only each recorded pin, several dependencies at a time.
    # Source preparation reads only the pinned trees, never their history.
    options = ["--depth", "1", "--jobs", "4"] if shallow else []
    return ["git", "-C", str(repo), "-c", "submodule.recurse=false",
            "submodule", "update", "--init", "--checkout", *options, "--", *paths]


def update(repo: Path, paths: tuple[str, ...], shallow: bool) -> None:
    command = update_command(repo, paths, shallow)
    attempts = len(RETRY_DELAYS) + 1
    for attempt, delay in enumerate((*RETRY_DELAYS, None), start=1):
        if subprocess.run(command).returncode == 0:
            return
        if delay is None:
            break
        print(f"Dependency checkout failed (attempt {attempt} of {attempts}); retrying in {delay} s",
              file=sys.stderr, flush=True)
        time.sleep(delay)
    raise SetupError("Dependency checkout failed; fix the Git error above and rerun setup")


def verify(repo: Path, expected: dict[str, str]) -> None:
    for path, commit in expected.items():
        check_existing(repo / path)
        if git(repo / path, "rev-parse", "HEAD").strip() != commit:
            raise SetupError(f"Dependency did not reach its recorded pin: {repo / path}")


def setup(repo: Path, *, launcher: bool = False, system: str | None = None,
          dry_run: bool = False, shallow: bool = False, switch: bool = False) -> None:
    repo = repo.resolve()
    if Path(git(repo, "rev-parse", "--show-toplevel").strip()).resolve() != repo:
        raise SetupError("Run the helper from a Git checkout of recharged")
    direct, nested = plan(launcher, system or platform.system(), switch)
    expected = pins(repo, direct)
    check_gitmodules(repo, direct)
    # Inspect every already initialized selected checkout before changing any.
    for path in direct:
        check_existing(repo / path)
    for parent, paths in nested:
        for path in paths:
            check_existing(repo / parent / path)
    if dry_run:
        for parent, paths in (("", direct), *nested):
            print(shlex.join(update_command(repo / parent, paths, shallow)))
        return
    update(repo, direct, shallow)
    verify(repo, expected)
    for parent, paths in nested:
        child = repo / parent
        expected_nested = pins(child, paths)
        check_gitmodules(child, paths)
        for path in paths:
            check_existing(child / path)
        update(child, paths, shallow)
        verify(child, expected_nested)
    build = "Switch" if switch else "launcher" if launcher else "game runtime"
    print(f"Pinned {build} dependencies are ready.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--launcher", action="store_true", help="Initialize only the smaller launcher build's dependencies")
    selection.add_argument("--switch", action="store_true", help="Initialize only the Nintendo Switch build's dependencies")
    parser.add_argument("--shallow", action="store_true",
                        help="Fetch only the pinned commits, without history (faster, for CI)")
    parser.add_argument("--dry-run", action="store_true", help="Check existing tracked edits and print the selected Git commands")
    args = parser.parse_args()
    try:
        setup(Path(__file__).resolve().parents[1], launcher=args.launcher, dry_run=args.dry_run,
              shallow=args.shallow, switch=args.switch)
    except (SetupError, OSError) as error:
        print(f"Dependency setup failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
