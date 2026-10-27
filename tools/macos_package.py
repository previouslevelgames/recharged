#!/usr/bin/env python3
"""The macOS release packages, made from the two per-architecture packages.

stage  joins the arm64 and x86_64 packages of tools/package_build.py into one
       package folder, FOLDER/Recharged, whose programs hold both halves (lipo)
pack   archives the signed folder as the macos-universal package, and the two
       per-architecture packages again from its signed halves: launchers up to
       1.0.15 only update from those
check  unpacks packages and checks their signatures, halves and programs
dmg    makes the disk image players install from: the Recharged folder beside
       a link to Applications

tools/sign_macos.py signs and notarizes between stage and pack, and the image
after dmg. Requires Python 3.11+; pack, check and dmg run on macOS.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path, PurePosixPath

from package_build import (MACOS_APP, MACOS_APP_TICKET, PackageError, digest, package_readme,
                           write_tarball)

PACKAGE = "Recharged"
INPUTS = "packages.json"
ARCHITECTURES = ("arm64", "x86_64")
PROGRAM = MACOS_APP + "/Contents/MacOS/recharged"
TICKET = MACOS_APP + "/" + MACOS_APP_TICKET
# Written again for every package.
REWRITTEN = ("PACKAGE-README.md", "SOURCE-MANIFEST.json")
# Mach-O: 64-bit (little-endian hosts), and universal (fat) files.
MACHO_MAGIC = (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf")
# The build's identity: both halves must agree on it.
SAME_BUILD = ("version", "packaging_source_commit", "modules", "prepared")


def is_macho(data):
    return data[:4] in MACHO_MAGIC


def read_package(path):
    """A package's manifest and files {name: (bytes, mode)}, checked against its manifest."""
    path = Path(path)
    files, top = {}, None
    with tarfile.open(path, "r:gz") as archive:
        for member in archive.getmembers():
            name = PurePosixPath(member.name)
            if name.is_absolute() or ".." in name.parts or not (member.isdir() or member.isfile()):
                raise PackageError(f"Unsafe entry in {path.name}: {member.name}")
            top = top or name.parts[0]
            if name.parts[0] != top:
                raise PackageError(f"{path.name} has more than one top folder")
            if member.isfile() and len(name.parts) > 1:
                files[PurePosixPath(*name.parts[1:]).as_posix()] = (archive.extractfile(member).read(),
                                                                   member.mode & 0o777)
    if "SOURCE-MANIFEST.json" not in files:
        raise PackageError(f"{path.name} has no SOURCE-MANIFEST.json")
    manifest = json.loads(files.pop("SOURCE-MANIFEST.json")[0])
    if set(manifest["files"]) != set(files):
        raise PackageError(f"{path.name} differs from its manifest: {sorted(set(manifest['files']) ^ set(files))}")
    for name, (data, mode) in files.items():
        record = manifest["files"][name]
        if record["sha256"] != digest(data) or record["bytes"] != len(data) or int(record["mode"], 8) != mode:
            raise PackageError(f"{path.name}: {name} differs from its manifest")
    return manifest, files


def retried(*args, attempts=4):
    """A step that asks Apple's servers (the ticket, Gatekeeper): their connection
    sometimes drops, so it runs again after a pause before it fails."""
    for attempt in range(1, attempts + 1):
        if subprocess.run([str(arg) for arg in args], check=False).returncode == 0:
            return
        if attempt < attempts:
            print(f"{args[0]} {args[1]} failed; trying again ({attempt}/{attempts - 1}).")
            time.sleep(15 * attempt)
    raise SystemExit(f"{' '.join(map(str, args[:2]))} failed {attempts} times")


def lipo(slices, output):
    subprocess.run(["lipo", "-create", *map(str, slices), "-output", str(output)], check=True)


def lipo_thin(source, architecture, output):
    subprocess.run(["lipo", str(source), "-thin", architecture, "-output", str(output)], check=True)


def stage(arm64, x86_64, folder, merge=lipo):
    """Writes the universal package folder, folder/Recharged; returns its path.
    `merge` joins two program halves (lipo by default)."""
    packages = {}
    for path in (arm64, x86_64):
        manifest, files = read_package(path)
        if manifest.get("platform") != "macos":
            raise PackageError(f"Not a macOS package: {Path(path).name}")
        packages[manifest["architecture"]] = (manifest, files)
    if set(packages) != set(ARCHITECTURES):
        raise PackageError("Expected the arm64 and the x86_64 macOS package")
    (first, first_files), (second, second_files) = (packages[architecture] for architecture in ARCHITECTURES)
    if (any(first[key] != second[key] for key in SAME_BUILD)
            or first["cargo"]["lock_sha256"] != second["cargo"]["lock_sha256"]):
        raise PackageError("The macOS packages come from different builds")
    programs = {PROGRAM, *first["modules"]}
    for files in (first_files, second_files):
        found = {name for name, (data, _) in files.items() if is_macho(data)}
        if found != programs:
            raise PackageError(f"Unexpected programs in a macOS package: {sorted(found ^ programs)}")
    names = (set(first_files) | set(second_files)) - programs - set(REWRITTEN)
    for name in sorted(names):
        if name in first_files and name in second_files:
            if first_files[name] != second_files[name]:
                raise PackageError(f"The macOS packages differ in {name}")
        elif not name.startswith("LICENSES/rust/"):
            # Only the Rust crates can differ (a target's own dependencies).
            raise PackageError(f"Only one macOS package has {name}")
    folder = Path(folder)
    if folder.exists():
        raise PackageError(f"The staging folder exists already: {folder}")
    root = folder / PACKAGE
    for name in sorted(names):
        data, mode = first_files.get(name) or second_files[name]
        write(root / name, data, mode)
    with tempfile.TemporaryDirectory() as temporary:
        for name in sorted(programs):
            halves = []
            for architecture in ARCHITECTURES:
                half = Path(temporary) / architecture / name
                half.parent.mkdir(parents=True, exist_ok=True)
                half.write_bytes(packages[architecture][1][name][0])
                halves.append(half)
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            merge(halves, root / name)
            (root / name).chmod(first_files[name][1])
    inputs = {architecture: packages[architecture][0] for architecture in ARCHITECTURES}
    (folder / INPUTS).write_text(json.dumps(inputs, indent=2, sort_keys=True) + "\n")
    return root


def write(path, data, mode):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    path.chmod(mode)


def read_signature(root):
    """How the folder's app and modules are signed, from codesign."""
    inputs = json.loads((root.parent / INPUTS).read_text())
    seals = set()
    for item in [MACOS_APP, *inputs["arm64"]["modules"]]:
        report = subprocess.run(["codesign", "-dvv", str(root / item)], capture_output=True, text=True,
                                check=True).stderr
        authority = re.search(r"^Authority=(.+)$", report, re.MULTILINE)
        team = re.search(r"^TeamIdentifier=([A-Z0-9]+)$", report, re.MULTILINE)
        seals.add((authority[1] if authority else "ad hoc", team[1] if team else None))
    if len(seals) != 1:
        raise PackageError(f"The app and its modules are signed differently: {sorted(seals, key=str)}")
    identity, team = seals.pop()
    return {"identity": identity, "team": team, "notarized": (root / TICKET).is_file()}


def pack(folder, output, signature=read_signature, thin=lipo_thin):
    """Archives the signed folder: the universal package, then the two
    per-architecture ones. Returns the archives' paths."""
    folder, output = Path(folder), Path(output)
    root = folder / PACKAGE
    inputs = json.loads((folder / INPUTS).read_text())
    files = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise PackageError(f"Symlink in the package folder: {path}")
        if path.is_file():
            files[path.relative_to(root).as_posix()] = (path.read_bytes(), path.stat().st_mode & 0o777)
    expected = set().union(*(inputs[architecture]["files"] for architecture in ARCHITECTURES)) - set(REWRITTEN)
    if set(files) - {TICKET} != expected:
        raise PackageError(f"Unexpected package folder contents: {sorted((set(files) - {TICKET}) ^ expected)}")
    seal = signature(root)
    first, second = (inputs[architecture] for architecture in ARCHITECTURES)
    version = first["version"]
    output.mkdir(parents=True, exist_ok=True)
    crates = {(item["name"], item["version"]): item
              for manifest in (first, second) for item in manifest["cargo"]["packages"]}
    universal = {key: first[key] for key in SAME_BUILD}
    universal.update({
        "format": 1, "platform": "macos", "architecture": "universal",
        "cargo": {"lock_sha256": first["cargo"]["lock_sha256"],
                  "options": {architecture: inputs[architecture]["cargo"]["options"] for architecture in ARCHITECTURES},
                  "packages": [crates[key] for key in sorted(crates)]},
        "macos_signature": seal})
    archives = [archive(output, f"recharged-{version}-macos-universal", universal, files,
                        package_readme(version, "macos", "universal", signature=seal))]
    programs = {PROGRAM, *first["modules"]}
    with tempfile.TemporaryDirectory() as temporary:
        for architecture in ARCHITECTURES:
            manifest = inputs[architecture]
            chosen = {}
            for name in manifest["files"]:
                if name in REWRITTEN:
                    continue
                if name in programs:
                    half = Path(temporary) / architecture / name
                    half.parent.mkdir(parents=True, exist_ok=True)
                    thin(root / name, architecture, half)
                    chosen[name] = (half.read_bytes(), files[name][1])
                else:
                    chosen[name] = files[name]
            if TICKET in files:
                chosen[TICKET] = files[TICKET]
            single = {key: value for key, value in manifest.items() if key != "files"}
            single["macos_signature"] = seal
            archives.append(archive(output, f"recharged-{version}-macos-{architecture}", single, chosen,
                                    package_readme(version, "macos", architecture, signature=seal)))
    return archives


def archive(output, package_name, manifest, files, readme):
    """Writes output/package_name.tar.gz as tools/package_build.py does."""
    files = {**files, "PACKAGE-README.md": (readme.encode(), 0o644)}
    manifest = {**manifest, "files": {name: {"sha256": digest(data), "bytes": len(data), "mode": oct(mode)}
                                      for name, (data, mode) in sorted(files.items())}}
    destination = output / (package_name + ".tar.gz")
    with tempfile.TemporaryDirectory(prefix=".recharged-package-", dir=output) as temporary:
        stage_dir = Path(temporary) / package_name
        for name, (data, mode) in files.items():
            write(stage_dir / name, data, mode)
        # The manifest describes the payload before itself, avoiding a self-hash.
        write(stage_dir / "SOURCE-MANIFEST.json", (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode(),
              0o644)
        temporary_archive = Path(temporary) / "package.tar.gz"
        write_tarball(temporary_archive, stage_dir, package_name)
        os.replace(temporary_archive, destination)
    return destination


def check(paths):
    """Unpacks each package and checks its programs: halves, signatures, notarization, --version."""
    for path in map(Path, paths):
        manifest, _ = read_package(path)
        architecture = manifest["architecture"]
        halves = set(ARCHITECTURES) if architecture == "universal" else {architecture}
        seal = manifest.get("macos_signature") or {}
        with tempfile.TemporaryDirectory() as temporary:
            with tarfile.open(path, "r:gz") as tar:
                tar.extractall(temporary, filter="data")
            root = next(Path(temporary).iterdir())
            for name in [PROGRAM, *manifest["modules"]]:
                found = set(subprocess.run(["lipo", "-archs", str(root / name)], capture_output=True, text=True,
                                           check=True).stdout.split())
                if found != halves:
                    raise PackageError(f"{path.name}: {name} holds {sorted(found)}, not {sorted(halves)}")
            for item in [MACOS_APP, *manifest["modules"]]:
                subprocess.run(["codesign", "--verify", "--strict", "--verbose=2", str(root / item)], check=True)
            if seal.get("notarized"):
                retried("xcrun", "stapler", "validate", root / MACOS_APP)
                retried("spctl", "--assess", "--type", "execute", "--verbose=2", root / MACOS_APP)
            for half in sorted(halves):
                report = subprocess.run(["arch", "-" + half, str(root / PROGRAM), "--version"], capture_output=True,
                                        text=True, check=True).stdout
                if "original frontend runtime" not in report:
                    raise PackageError(f"{path.name}: the {half} program reports no game runtime:\n{report}")
        print(f"{path.name}: {', '.join(sorted(halves))}; {seal.get('identity', 'unsigned')}"
              + ("; notarized" if seal.get("notarized") else ""), file=sys.stderr)


# On the disk image, beside the folder: the folder holds the player's settings
# and saves once installed, so a newer image must not replace it.
UPDATE_NOTE = """Recharged keeps your settings and saves in its folder.

Already installed? Recharged updates itself: its launcher offers each new
version. Do not replace your Recharged folder with the one on this disk image,
that would delete your settings and saves. To update by hand, copy the files
from this Recharged folder into yours instead.
"""


def dmg(folder, output):
    """The disk image: the Recharged folder beside a link to Applications, to drag it onto."""
    folder, output = Path(folder), Path(output)
    version = json.loads((folder / INPUTS).read_text())["arm64"]["version"]
    destination = output / f"recharged-{version}-macos-universal.dmg"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        volume = Path(temporary) / "volume"
        volume.mkdir()
        # ditto keeps the signatures and the stapled ticket exactly as they are.
        # The tools report to the log: this command's output is only the image's path.
        subprocess.run(["ditto", str(folder / PACKAGE), str(volume / PACKAGE)], check=True, stdout=sys.stderr)
        (volume / "Applications").symlink_to("/Applications")
        (volume / "Already installed.txt").write_text(UPDATE_NOTE)
        subprocess.run(["hdiutil", "create", "-volname", "Recharged", "-srcfolder", str(volume), "-fs", "HFS+",
                        "-format", "UDZO", "-ov", str(destination)], check=True, stdout=sys.stderr)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    joined = commands.add_parser("stage")
    joined.add_argument("arm64", type=Path)
    joined.add_argument("x86_64", type=Path)
    joined.add_argument("folder", type=Path)
    for name in ("pack", "dmg"):
        command = commands.add_parser(name)
        command.add_argument("folder", type=Path)
        command.add_argument("output", type=Path)
    checked = commands.add_parser("check")
    checked.add_argument("packages", type=Path, nargs="+")
    args = parser.parse_args()
    try:
        if args.command == "stage":
            print(stage(args.arm64, args.x86_64, args.folder))
        elif args.command == "pack":
            print("\n".join(map(str, pack(args.folder, args.output))))
        elif args.command == "check":
            check(args.packages)
        else:
            print(dmg(args.folder, args.output))
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"macOS packaging failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
