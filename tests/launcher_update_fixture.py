#!/usr/bin/env python3
"""Packages for tests/launcher_updates.cpp, written like tools/package_build.py.

Usage: launcher_update_fixture.py TEST_BINARY OUTPUT_DIR

Creates an installed 1.0.9 package with user files beside it, 1.0.10 packages
(tar.gz with a top folder for Linux, the Steam Deck and the universal Mac
package, zip with files at its root for Windows), an installed arm64 Mac
folder, and damaged or hostile archives; then runs the C++ checks.
"""

import hashlib
import io
import json
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

OLD_VERSION = "1.0.9+gaaaaaaaaaaaa"
NEW_VERSION = "1.0.10+gbbbbbbbbbbbb"

OLD_FILES = {
    "mscharged": (b"old launcher\n", 0o755),
    "mscharged_original_frontend_module.so": (b"old module\n", 0o644),
    "assets/launcher/header.png": (b"old header\n", 0o644),
    "docs/OLD.md": (b"only in the old package\n", 0o644),
}
NEW_FILES = {  # renamed to recharged (#64)
    "recharged": (b"new launcher\n", 0o755),
    "recharged_game_us.so": (b"new module\n" * 1000, 0o644),
    "assets/launcher/header.png": (b"new header\n", 0o644),
    "docs/NEW.md": (b"only in the new package\n", 0o644),
    # A path longer than ustar's 100-character name field (a pax header).
    "LICENSES/dependencies/" + "a" * 60 + "/" + "b" * 60 + "/LICENSE": (b"license text\n", 0o644),
}
USER_FILES = {
    "mscharged.ini": b"[game]\ndisc = game/R4QE01.rvz\n",
    "data/nand/save.bin": b"save data",
    "logs/mscharged.log": b"previous log\n",
}


def manifest(version, platform, files, architecture="x86_64"):
    return json.dumps({
        "format": 1, "version": version, "platform": platform, "architecture": architecture,
        "files": {path: {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data), "mode": oct(mode)}
                  for path, (data, mode) in files.items()},
    }, indent=2, sort_keys=True).encode() + b"\n"


def stage(root, version, platform, files, architecture="x86_64"):
    root.mkdir(parents=True)
    for path, (data, mode) in files.items():
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(mode)
    (root / "SOURCE-MANIFEST.json").write_bytes(manifest(version, platform, files, architecture))


def write_tar(archive, stage_dir, package_name):
    # As tools/package_build.py: the folder first, then its contents sorted.
    with tarfile.open(archive, "w:gz") as tar:
        for path in [stage_dir] + sorted(stage_dir.rglob("*")):
            info = tar.gettarinfo(str(path), str(Path(package_name) / path.relative_to(stage_dir)))
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            if path.is_file():
                with path.open("rb") as stream:
                    tar.addfile(info, stream)
            else:
                tar.addfile(info)


def write_zip(archive, stage_dir):
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zipped:
        for path in sorted(stage_dir.rglob("*")):
            if path.is_file():
                zipped.write(path, path.relative_to(stage_dir).as_posix())


def tar_with(archive, members):
    """A tar.gz of explicit members: (TarInfo, bytes or None)."""
    with tarfile.open(archive, "w:gz") as tar:
        for info, data in members:
            tar.addfile(info, io.BytesIO(data) if data is not None else None)


def file_info(name, data, mode=0o644):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    return info


def main():
    binary, output = Path(sys.argv[1]), Path(sys.argv[2])
    shutil.rmtree(output, ignore_errors=True)
    output.mkdir(parents=True)
    work = output / "stage"

    installed = output / "installed"
    stage(installed, OLD_VERSION, "linux", OLD_FILES)
    for path, data in USER_FILES.items():
        (installed / path).parent.mkdir(parents=True, exist_ok=True)
        (installed / path).write_bytes(data)

    for platform in ("linux", "steamdeck"):
        name = f"recharged-{NEW_VERSION}-{platform}-x86_64"
        stage(work / name, NEW_VERSION, platform, NEW_FILES)
        write_tar(output / f"{name}.tar.gz", work / name, name)
    name = f"recharged-{NEW_VERSION}-windows-x86_64"
    stage(work / name, NEW_VERSION, "windows", NEW_FILES)
    write_zip(output / f"{name}.zip", work / name)
    # An Apple Silicon Mac's folder, and the universal Mac package for it.
    stage(output / "installed-macos", OLD_VERSION, "macos", OLD_FILES, "arm64")
    name = f"recharged-{NEW_VERSION}-macos-universal"
    stage(work / name, NEW_VERSION, "macos", NEW_FILES, "universal")
    write_tar(output / f"{name}.tar.gz", work / name, name)

    top = "recharged-1.0.10+gbbbbbbbbbbbb-linux-x86_64"
    good_manifest = manifest(NEW_VERSION, "linux", {"recharged": (b"x\n", 0o755)})
    tar_with(output / "bad-traversal.tar.gz", [
        (file_info(f"{top}/SOURCE-MANIFEST.json", good_manifest), good_manifest),
        (file_info(f"{top}/../../evil", b"escaped\n"), b"escaped\n"),
    ])
    link = tarfile.TarInfo(f"{top}/recharged")
    link.type = tarfile.SYMTYPE
    link.linkname = "/bin/sh"
    tar_with(output / "bad-symlink.tar.gz", [
        (file_info(f"{top}/SOURCE-MANIFEST.json", good_manifest), good_manifest), (link, None)])
    tar_with(output / "bad-hash.tar.gz", [
        (file_info(f"{top}/SOURCE-MANIFEST.json", good_manifest), good_manifest),
        (file_info(f"{top}/recharged", b"y\n", 0o755), b"y\n"),
    ])
    tar_with(output / "bad-absolute.tar.gz", [(file_info("/tmp/recharged-absolute", b"x\n"), b"x\n")])
    unlisted = work / "unlisted"
    stage(unlisted, NEW_VERSION, "windows", {"recharged.exe": (b"x\n", 0o755)})
    (unlisted / "extra.dll").write_bytes(b"not in the manifest\n")
    write_zip(output / "bad-unlisted.zip", unlisted)
    complete = (output / f"recharged-{NEW_VERSION}-linux-x86_64.tar.gz").read_bytes()
    (output / "bad-truncated.tar.gz").write_bytes(complete[: len(complete) // 2])
    shutil.rmtree(work)

    return subprocess.run([str(binary), str(output)], check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
