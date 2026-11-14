#!/usr/bin/env python3
"""Make Recharged.app and an unsigned .ipa from an ios-release build, without game data.

The .ipa is signed when it is installed (Sideloadly, AltStore or Xcode with the
player's Apple ID). Run on the Mac that built it: the icon sizes come from `sips`
and the SDK keys from `xcrun` when they exist.

Layout of Recharged.app (SDL_GetBasePath is the app folder itself):
  recharged, Info.plist, PkgInfo, AppIcon*.png
  Frameworks/recharged_game_{us,eu,jp,kr}.so   the game modules (one per game version;
                                                 --regions keeps only those built)
  assets/, README.md, docs/...                   what every package holds beside its program
                                                 (package_build.py's common_files)
  LICENSE, LICENSES/...                          this port's and the dependencies' notices,
                                                 with each Rust crate's in LICENSES/rust
"""

import argparse
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import tomllib
import zipfile
from pathlib import Path

from package_build import (DEPENDENCIES, MODULES, PackageError, cache_values, cached_crate, cargo_packages,
                           common_files, notice_paths, regular_file)
from pipeline_catalog import CatalogError, catalog_rows, verify_database

APP = "Recharged.app"
EXECUTABLE = "recharged"
# CFBundleIconFiles in cmake/Info-iOS.plist.in: name -> (file, pixels).
ICONS = (("AppIcon60x60@2x.png", 120), ("AppIcon60x60@3x.png", 180),
         ("AppIcon76x76@2x~ipad.png", 152), ("AppIcon83.5x83.5@2x~ipad.png", 167))
# Never in a package: disc images and the game's own files (see .gitignore).
GAME_DATA = re.compile(r"\.(iso|gcm|rvz|wia|wbfs|ciso|gcz|tgc|nfs|dol|elf)$", re.IGNORECASE)


def tool(*command):
    """A tool's output, or None where it is missing (packaging on a non-Mac for a check)."""
    if not shutil.which(command[0]):
        return None
    result = subprocess.run(command, capture_output=True, check=False)
    if result.returncode:
        raise PackageError(f"{command[0]} failed: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout.decode(errors="replace")


def check_ios_binary(path):
    """The program and modules must be arm64 Mach-O for iOS devices, not macOS."""
    archs = tool("lipo", "-archs", str(path))
    if archs is not None and archs.split() != ["arm64"]:
        raise PackageError(f"{path.name} is built for {archs.strip()}, not arm64")
    commands = tool("otool", "-l", str(path))
    if commands is not None:
        # LC_BUILD_VERSION platform 2 is iOS (1: macOS, 7: iOS simulator).
        platforms = set(re.findall(r"^\s*platform (\S+)$", commands, re.MULTILINE))
        if platforms and not platforms <= {"2", "IOS"}:
            raise PackageError(f"{path.name} is not built for iOS devices (platform {', '.join(sorted(platforms))})")


def build_version(build):
    body = regular_file(build, "generated/mscharged/build_version.h").read_text()
    match = re.search(r'const char\* version\s*=\s*"([A-Za-z0-9._+\-]+)"', body)
    if not match:
        raise PackageError("Missing or invalid embedded build version.")
    return match[1]


def info_plist(build):
    info = plistlib.loads(regular_file(build, "generated/mscharged/Info-iOS.plist").read_bytes())
    sdk_version = tool("xcrun", "--sdk", "iphoneos", "--show-sdk-version")
    sdk_build = tool("xcrun", "--sdk", "iphoneos", "--show-sdk-build-version")
    if sdk_version:
        info["DTPlatformName"] = "iphoneos"
        info["DTPlatformVersion"] = sdk_version.strip()
        info["DTSDKName"] = "iphoneos" + sdk_version.strip()
    if sdk_build:
        info["DTSDKBuild"] = sdk_build.strip()
        info["DTPlatformBuild"] = sdk_build.strip()
    return plistlib.dumps(info, fmt=plistlib.FMT_XML)


REGIONS = ("us", "eu", "jp", "kr")


def package_ios(build, output, source, regions=REGIONS):
    build, output, source = build.resolve(), output.resolve(), source.resolve()
    cache = regular_file(build, "CMakeCache.txt").read_text()
    if not re.search(r"^CMAKE_SYSTEM_NAME:\w+=iOS$", cache, re.MULTILINE):
        raise PackageError("Not an iOS build (configure with the ios-release preset).")
    version = build_version(build)
    executable = regular_file(build, EXECUTABLE)
    check_ios_binary(executable)
    modules = []
    for stem in MODULES:
        if stem.rsplit("_", 1)[1] not in regions:
            continue
        matches = [stem + suffix for suffix in (".so", ".dylib", ".bundle") if (build / (stem + suffix)).exists()]
        if len(matches) != 1:
            raise PackageError(f"Require exactly one loadable {stem} module, found {len(matches)}.")
        check_ios_binary(regular_file(build, matches[0]))
        modules += matches
    catalog = regular_file(source, "assets/graphics/pipelines.json")
    pipeline_resource = regular_file(build, "assets/graphics/initial_pipeline_cache.db")
    try:
        verify_database(pipeline_resource, catalog_rows(catalog))
    except CatalogError as error:
        raise PackageError(str(error)) from error

    output.mkdir(parents=True, exist_ok=True)
    name = f"recharged-{version}-ios-arm64"
    with tempfile.TemporaryDirectory(prefix=".recharged-ios-", dir=output) as temporary:
        app = Path(temporary) / "Payload" / APP
        app.mkdir(parents=True)

        def copy(root, relative, destination=None):
            path = regular_file(root, relative)
            target = app / (destination or relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
            target.chmod(stat.S_IMODE(path.stat().st_mode) | 0o644)

        copy(build, EXECUTABLE)
        (app / EXECUTABLE).chmod(0o755)
        (app / "Info.plist").write_bytes(info_plist(build))
        (app / "PkgInfo").write_bytes(b"APPL????")
        icon = regular_file(source, "assets/launcher/icon.png")
        for file, pixels in ICONS:
            shutil.copyfile(icon, app / file)
            tool("sips", "-z", str(pixels), str(pixels), str(app / file))
        for module in modules:
            copy(build, module, Path("Frameworks") / module)
            (app / "Frameworks" / module).chmod(0o755)
        # What every package holds beside its program (package_build.py).
        for root, relative in common_files(build, source):
            copy(root, relative)
        for dependency in DEPENDENCIES:
            root = build / "prepared" / dependency
            if dependency == "mscharged-decomp":
                # Its prepared export holds only include/libs/src; the notice is the checkout's.
                copy(source / "extern/mscharged-decomp", "LICENSE", Path("LICENSES/dependencies") / dependency / "LICENSE")
                continue
            for path in notice_paths(root / "source"):
                copy(root / "source", path, Path("LICENSES/dependencies") / dependency / path)
        copy(build / "prepared/nod/source", "Cargo.lock", "LICENSES/rust/Cargo.lock")
        rust_notices(build, app)

        stray = [path for path in app.rglob("*") if GAME_DATA.search(path.name)]
        if stray:
            raise PackageError(f"Game data must never be packaged: {', '.join(map(str, stray))}")

        # The .ipa is a zip of Payload/; the permission bits must survive it.
        archive = Path(temporary) / "package.ipa"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as ipa:
            for path in sorted((Path(temporary) / "Payload").rglob("*")):
                relative = path.relative_to(temporary).as_posix()
                info = zipfile.ZipInfo(relative + ("/" if path.is_dir() else ""))
                info.external_attr = (stat.S_IMODE(path.stat().st_mode) | (0o40000 if path.is_dir() else 0o100000)) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                ipa.writestr(info, b"" if path.is_dir() else path.read_bytes())
        destination = output / (name + ".ipa")
        archive.replace(destination)
        shutil.rmtree(output / APP, ignore_errors=True)
        shutil.copytree(app, output / APP, symlinks=True)
    return destination


def rust_notices(build, app):
    """Each Rust crate's own license texts in LICENSES/rust/<crate>-<version>,
    as the desktop packages have them (package_build.py): the crates the build
    resolved for nod-ffi on its target, read from the Cargo cache it used.
    nod and nod-ffi themselves are covered by LICENSES/dependencies/nod."""
    packages, _ = cargo_packages(build, cache_values(build))
    lock = tomllib.loads(regular_file(build / "prepared/nod/source", "Cargo.lock").read_text())
    locked = {(p["name"], p["version"], p.get("source")): p for p in lock["package"]}
    for package in packages:
        if not package.get("source"):
            continue
        key = (package["name"], package["version"], package["source"])
        checksum = locked.get(key, {}).get("checksum")
        if not checksum:
            raise PackageError(f"Rust package without a locked checksum: {key[:2]}")
        _, notices = cached_crate(Path(package["crate_path"]), key[0], key[1], checksum)
        if not notices:
            raise PackageError(f"No cached Rust license texts: {key[:2]}")
        destination = app / "LICENSES/rust" / f"{key[0]}-{key[1]}"
        for path, data in notices.items():
            target = destination / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            target.chmod(0o644)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, default=Path("build/ios-release"))
    parser.add_argument("--output-dir", type=Path, default=Path("build/ios-artifacts"))
    parser.add_argument("--regions", nargs="+", default=["all"], metavar="REGION",
                        help="Game modules to package: us, eu, jp, kr or all (default: all).")
    parser.add_argument("--source-dir", type=Path, default=Path(__file__).resolve().parents[1],
                        help="Port checkout (defaults to the script's repository).")
    args = parser.parse_args()
    try:
        regions = REGIONS if args.regions == ["all"] else tuple(dict.fromkeys(args.regions))
        if unknown := [region for region in regions if region not in REGIONS]:
            raise PackageError(f"Unknown region: {', '.join(unknown)} (use us, eu, jp, kr or all)")
        ipa = package_ios(args.build_dir, args.output_dir, args.source_dir, regions)
    except (OSError, ValueError, KeyError) as error:
        print(f"Packaging failed: {error}", file=sys.stderr)
        return 1
    print(ipa)
    return 0


if __name__ == "__main__":
    sys.exit(main())
