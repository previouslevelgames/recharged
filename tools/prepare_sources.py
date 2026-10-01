#!/usr/bin/env python3
"""Export pinned submodules and apply ordered patches without editing upstream."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


class PreparationError(RuntimeError):
    pass


def git(repo: Path, *args: str, allowed=(0,)) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(repo), *args], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, env={**os.environ, "GIT_OPTIONAL_LOCKS": "0"},
    )
    if result.returncode not in allowed:
        raise PreparationError(result.stderr.decode(errors="replace").strip()
                               or f"git {' '.join(args)} failed")
    return result.stdout


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def encoded(value) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def safe_child(root: Path, name: str) -> Path:
    result = (root / name).resolve()
    if not result.is_relative_to(root.resolve()) or result == root.resolve():
        raise PreparationError(f"Path escapes its source directory: {name}")
    return result


def submodule_inputs(root: Path, name: str):
    if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_.-]*", name):
        raise PreparationError(f"Invalid dependency name: {name}")
    relative = f"extern/{name}"
    entries = git(root, "ls-files", "--stage", "-z", "--", relative).split(b"\0")
    entries = [entry for entry in entries if entry]
    if len(entries) != 1:
        raise PreparationError(f"{relative} must have one recorded submodule pin in the index")
    metadata, path = entries[0].split(b"\t", 1)
    mode, commit, stage = metadata.decode().split()
    if mode != "160000" or stage != "0" or path.decode() != relative:
        raise PreparationError(f"{relative} is not an unmerged-free Git submodule pin")
    declared = git(root, "config", "--file", ".gitmodules", "--get-regexp",
                   r"^submodule\..*\.path$", allowed=(0, 1)).decode().splitlines()
    if not any(line.split(maxsplit=1)[1] == relative for line in declared):
        raise PreparationError(f"{relative} is missing from .gitmodules")
    sources = []

    def visit(repo: Path, expected: str, prefix: str):
        if not (repo / ".git").exists():
            raise PreparationError(f"Initialize the pinned submodule first: {repo}")
        if Path(git(repo, "rev-parse", "--show-toplevel").decode().strip()).resolve() != repo.resolve():
            raise PreparationError(f"Not an independent submodule checkout: {repo}")
        actual = git(repo, "rev-parse", "HEAD").decode().strip()
        if actual != expected:
            raise PreparationError(f"Unexpected revision in {repo}: {actual}; expected {expected}")
        dirty = git(repo, "status", "--porcelain=v1", "--untracked-files=no", "--ignore-submodules=none")
        if dirty:
            raise PreparationError(f"Tracked changes in {repo}; preserve your edits before preparing sources")
        sources.append({"path": prefix, "commit": expected, "repo": repo})
        for entry in git(repo, "ls-tree", "-rz", expected).split(b"\0"):
            if not entry:
                continue
            metadata, path = entry.split(b"\t", 1)
            mode, kind, oid = metadata.decode().split()
            if kind == "commit":
                child = path.decode()
                visit(safe_child(repo, child), oid, f"{prefix}/{child}".strip("/"))

    visit(safe_child(root, relative), commit, "")
    return sources


def patch_inputs(root: Path, name: str, commit: str):
    directory = root / "patches" / name
    if not directory.exists():
        return [], None
    base = (directory / "base").read_text().strip()
    if base != commit:
        raise PreparationError(f"Patch base for {name} is {base}; selected pin is {commit}. Refresh the series explicitly.")
    series = (directory / "series").read_bytes()
    patches = []
    for line in series.decode().splitlines():
        name = line.strip()
        if not name or name.startswith("#"):
            continue
        path = safe_child(directory, name)
        if any(p["name"] == name for p in patches):
            raise PreparationError(f"Duplicate patch in series: {name}")
        patches.append({"name": name, "sha256": sha(path.read_bytes()), "file": path})
    return patches, sha(series)


def content_inventory(directory: Path):
    result = {}
    for parent, directories, files in os.walk(directory, followlinks=False):
        for name in sorted(directories + files):
            path = Path(parent) / name
            relative = path.relative_to(directory).as_posix()
            if path.is_symlink():
                result[relative] = {"symlink": os.readlink(path)}
            elif path.is_file():
                result[relative] = {"sha256": sha(path.read_bytes()),
                                    "executable": bool(path.stat().st_mode & 0o111)}
    return result


def export_committed(repo: Path, commit: str, destination: Path):
    # Read tree/blob objects directly: checkout filters and local or committed
    # export-ignore/export-subst attributes must not change this source snapshot.
    entries = git(repo, "ls-tree", "-rz", commit).split(b"\0")
    with subprocess.Popen(["git", "-C", str(repo), "cat-file", "--batch"],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL) as reader:
        for entry in entries:
            if not entry:
                continue
            metadata, raw_path = entry.split(b"\t", 1)
            mode, kind, oid = metadata.decode().split()
            if kind == "commit":
                continue  # Nested repositories are exported from their own pins.
            if kind != "blob":
                raise PreparationError(f"Unsupported source object: {kind}")
            path = safe_child(destination, os.fsdecode(raw_path))
            if ".git" in path.relative_to(destination).parts:
                raise PreparationError("Upstream content may not replace Git metadata")
            reader.stdin.write((oid + "\n").encode())
            reader.stdin.flush()
            header = reader.stdout.readline().split()
            if len(header) != 3 or header[0].decode() != oid or header[1] != b"blob":
                raise PreparationError(f"Cannot read committed blob {oid}")
            size = int(header[2])
            data = reader.stdout.read(size)
            if len(data) != size or reader.stdout.read(1) != b"\n":
                raise PreparationError(f"Truncated committed blob {oid}")
            path.parent.mkdir(parents=True, exist_ok=True)
            if mode == "120000":
                link = os.fsdecode(data)
                if not (path.parent / link).resolve().is_relative_to(destination.resolve()):
                    raise PreparationError(f"Upstream symlink escapes exported source: {path}")
                path.symlink_to(link)
            else:
                path.write_bytes(data)
                path.chmod(int(mode, 8) & 0o777)
        reader.stdin.close()
        if reader.wait() != 0:
            raise PreparationError("Failed to read committed source objects")


def materialize(destination: Path, sources, patches, keep_git=False):
    destination.mkdir(parents=True)
    for source in sources:
        target = destination / source["path"]
        target.mkdir(parents=True, exist_ok=True)
        export_committed(source["repo"], source["commit"], target)
    # An isolated repository prevents `git apply` from discovering the parent
    # repository and silently treating these patches as outside the cwd prefix.
    git(destination, "-c", "init.defaultBranch=prepared", "init", "--quiet")
    git(destination, "config", "core.autocrlf", "false")
    for patch in patches:
        try:
            git(destination, "-c", "apply.ignoreWhitespace=false", "apply", "--check",
                "--whitespace=error-all", str(patch["file"]))
            git(destination, "-c", "apply.ignoreWhitespace=false", "apply",
                "--whitespace=error-all", str(patch["file"]))
        except PreparationError as error:
            raise PreparationError(f"Patch failed: {patch['name']}\n{error}") from error
    if not keep_git:
        shutil.rmtree(destination / ".git")


@contextmanager
def preparation_lock(directory: Path):
    try:
        directory.mkdir()
    except FileExistsError as error:
        raise PreparationError(f"Source preparation is already locked: {directory}. If a previous process crashed, remove this empty lock directory after checking it has stopped.") from error
    try:
        yield
    finally:
        directory.rmdir()


def prepare(root: Path, build: Path, name: str, *, check=False,
            discard_generated=False, export_patch: Path | None = None) -> Path:
    root, build = root.resolve(), build.resolve()
    if not build.is_relative_to(root) or build == root:
        raise PreparationError("Use an ignored build directory inside this checkout")
    first = build.relative_to(root).parts[0]
    if not (first in ("build", "out") or first.startswith(("build-", "cmake-build-"))):
        raise PreparationError("Build directory must be under build/, build-*/, cmake-build-*/, or out/")
    sources = submodule_inputs(root, name)
    patches, series_sha = patch_inputs(root, name, sources[0]["commit"])
    inputs = {
        "dependency": name, "sources": [{k: s[k] for k in ("path", "commit")} for s in sources],
        "patches": [{k: p[k] for k in ("name", "sha256")} for p in patches],
        "series_sha256": series_sha, "preparer_sha256": sha(Path(__file__).read_bytes()),
        "format": 1,
    }
    key = sha(encoded(inputs))
    parent = build / "prepared"
    parent.mkdir(parents=True, exist_ok=True)
    target = parent / name
    source_path = target / "source"
    with preparation_lock(parent / f".{name}.lock"):
        state = None
        if (target / "manifest.json").is_file():
            state = json.loads((target / "manifest.json").read_text())
        same_inputs = state is not None and state.get("key") == key
        clean_output = state is not None and source_path.is_dir() and state.get("content") == content_inventory(source_path)
        if export_patch is not None:
            if not same_inputs:
                raise PreparationError("Export requires the same pin, series, and preparer used to create this tree; restore those inputs before exporting")
            export_patch = export_patch.resolve()
            if export_patch.exists() or not export_patch.is_relative_to(build) or export_patch.is_relative_to(parent):
                raise PreparationError("Choose a new patch output file under the build directory, outside prepared/")
            with tempfile.TemporaryDirectory(prefix=f".{name}-export-", dir=parent) as temp:
                expected = Path(temp) / "source"
                materialize(expected, sources, patches, keep_git=True)
                git(expected, "add", "--force", "--all")
                worktree = ("--git-dir=" + str(expected / ".git"), "--work-tree=" + str(source_path))
                added = git(source_path, *worktree, "ls-files", "--others", "-z").split(b"\0")
                for path in added:
                    if path:
                        git(source_path, *worktree, "add", "--intent-to-add", "--force", "--", path.decode())
                diff = git(source_path, *worktree, "diff", "--binary", "--no-ext-diff", "--no-renames")
                if not diff:
                    raise PreparationError("No generated-source changes to export")
                export_patch.parent.mkdir(parents=True, exist_ok=True)
                export_patch.write_bytes(diff)
            return export_patch
        if check:
            if not same_inputs or not clean_output:
                raise PreparationError(f"Prepared sources for {name} are stale or modified; rerun CMake configuration. Export local source edits before regenerating.")
            return source_path
        if target.exists() and not clean_output and not discard_generated:
            raise PreparationError("Generated sources were edited or their manifest is missing. Use --export-patch first, then --discard-generated only when those edits are preserved.")
        if same_inputs and clean_output:
            return source_path
        with tempfile.TemporaryDirectory(prefix=f".{name}-prepare-", dir=parent) as temp:
            staged = Path(temp) / "ready"
            materialize(staged / "source", sources, patches)
            state = {"key": key, "inputs": inputs, "content": content_inventory(staged / "source")}
            (staged / "manifest.json").write_text(json.dumps(state, indent=2, sort_keys=True) + "\n")
            previous = Path(temp) / "previous"
            if target.exists():
                target.rename(previous)
            try:
                staged.rename(target)
            except OSError:
                if previous.exists():
                    previous.rename(target)
                raise
        return source_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependency", default="mscharged-decomp")
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--check", action="store_true", help="Verify pins, inputs, and generated contents; do not regenerate")
    modes.add_argument("--discard-generated", action="store_true", help="Explicitly allow replacing edited generated sources")
    modes.add_argument("--export-patch", type=Path, help="Export local edits as a new patch under the build directory")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    try:
        path = prepare(root, args.build_dir, args.dependency, check=args.check,
                       discard_generated=args.discard_generated, export_patch=args.export_patch)
        print(path)
    except (PreparationError, OSError, ValueError) as error:
        parser.exit(1, f"Source preparation failed: {error}\n")


if __name__ == "__main__":
    if sys.version_info < (3, 10):
        raise SystemExit("Source preparation requires Python 3.10 or newer")
    main()
