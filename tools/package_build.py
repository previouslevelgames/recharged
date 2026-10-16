#!/usr/bin/env python3
"""Package a completed original-game Linux/macOS/Windows build without game data.

--steamdeck packages a Linux x86_64 build for SteamOS (Steam Deck) instead: it
refuses binaries that need newer system libraries than SteamOS 3.7 provides.

Requires Python 3.11+, Git, and the Cargo cache used by the completed build.
Cargo tree runs locked and offline; packaging does not compile or download.
"""

import argparse
import hashlib
import io
import json
import os
import re
import stat
import subprocess
import sys
import tarfile
import tempfile
import tomllib
import zipfile
from pathlib import Path

from setup_windows_graphics import verify as verify_windows_graphics, file_records as graphics_file_records

from pipeline_catalog import CatalogError, catalog_rows, verify_database

WINDOWS_GRAPHICS_PIN = Path(__file__).with_name("windows_graphics.json")

# One game module per game version (USA, European), then the Credits diagnostic.
MODULES = ("mscharged_original_frontend_module_us", "mscharged_original_frontend_module_eu",
           "mscharged_original_main_credits_module")
ASSETS = ("header.png", "icon.png", "aurora.png", "Roboto-Medium.ttf", "README.md", "LICENSE-APACHE")
# LLVM-MinGW runtime DLLs a Windows build imports, and the notices that cover
# them (LLVM: libc++/libunwind; mingw-w64: winpthreads and CRT startup code).
WINDOWS_RUNTIME = ("libc++.dll", "libunwind.dll", "libwinpthread-1.dll")
WINDOWS_RUNTIME_NOTICES = ("LICENSE.TXT", "x86_64-w64-mingw32/share/mingw32/COPYING.winpthreads.txt",
                           "x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt")
# OS components supplied by supported Windows 10/11 installations. Compiler
# runtimes (including the Visual C++ runtime) deliberately are not in this set.
WINDOWS_SYSTEM_DLLS = frozenset((
    "advapi32.dll", "bcrypt.dll", "bcryptprimitives.dll", "cfgmgr32.dll", "comdlg32.dll",
    "crypt32.dll", "d3d11.dll", "d3d12.dll", "dbghelp.dll", "dwmapi.dll", "dxgi.dll",
    "gdi32.dll", "imm32.dll", "kernel32.dll", "ntdll.dll", "ole32.dll", "oleaut32.dll", "pdh.dll",
    "rpcrt4.dll", "setupapi.dll", "shell32.dll", "shlwapi.dll", "ucrtbase.dll",
    "user32.dll", "userenv.dll", "version.dll", "winhttp.dll", "winmm.dll", "wintrust.dll", "ws2_32.dll",
))
PUBLIC_FILES = ("README.md", "LICENSE", "mscharged.ini.example", "docs/INSTALL.md", "docs/STEAM_DECK.md",
                "docs/FEATURES.md", "docs/SETTINGS.md", "docs/TROUBLESHOOTING.md", "docs/ARCHITECTURE.md",
                "docs/BUILDING.md", "LICENSES/README.md", "LICENSES/Apache-2.0.txt",
                "extern/README.md")
# SteamOS 3.7 (Steam Deck): glibc 2.40 and the GCC 14 runtime (gcc-libs 14.2),
# from Valve's package mirror. A Steam Deck package needs no newer symbol
# versions and links only these system libraries; SDL and Dawn load the
# desktop, audio and Vulkan libraries at run time.
STEAMDECK_SYMBOL_CEILINGS = {"GLIBC": (2, 40), "GLIBCXX": (3, 4, 33), "CXXABI": (1, 3, 15), "GCC": (14, 0, 0)}
STEAMDECK_SYSTEM_LIBRARIES = frozenset((
    "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
    "libstdc++.so.6", "libgcc_s.so.1", "ld-linux-x86-64.so.2",
))
DEPENDENCIES = ("mscharged-decomp", "aurora", "sdl", "nod", "corrosion", "dawn",
                "imgui", "freetype", "libpng", "zlib-ng", "zstd", "sqlite",
                "tracy", "fmt", "xxhash")


class PackageError(ValueError):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


def regular_file(root, relative):
    path = root / relative
    if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(root.resolve()):
        raise PackageError(f"Missing or unsafe required file: {path}")
    return path


def run(command):
    result = subprocess.run(command, capture_output=True, check=False)
    if result.returncode:
        raise PackageError(f"{command[0]} failed: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout


def cache_values(build):
    values = {}
    for line in regular_file(build, "CMakeCache.txt").read_text().splitlines():
        match = re.match(r"([^/#][^:]*):[^=]+=(.*)$", line)
        if match:
            values[match[1]] = match[2]
    if values.get("MSCHARGED_BUILD_ORIGINAL_FRONTEND_DIAGNOSTIC", "").upper() not in ("ON", "TRUE", "1"):
        raise PackageError("The build must enable the original frontend runtime, not only the launcher.")
    return values


def target_platform(build):
    # Read the configured target, not the machine running this packaging script.
    systems = sorted(build.glob("CMakeFiles/*/CMakeSystem.cmake"))
    if not systems:
        raise PackageError("Missing configured CMake system metadata.")
    descriptions = set()
    for path in systems:
        body = regular_file(build, path.relative_to(build)).read_text()
        fields = dict(re.findall(r'set\((CMAKE_SYSTEM_NAME|CMAKE_SYSTEM_PROCESSOR) "([^"\n]+)"\)', body))
        descriptions.add((fields.get("CMAKE_SYSTEM_NAME"), fields.get("CMAKE_SYSTEM_PROCESSOR")))
    if len(descriptions) != 1:
        raise PackageError("Conflicting configured CMake target platforms; use a fresh build directory.")
    system, arch = descriptions.pop()
    if system not in ("Linux", "Darwin", "Windows") or not arch or not re.fullmatch(r"[A-Za-z0-9_+-]+", arch):
        raise PackageError(f"Packaging is supported for Linux, macOS and Windows builds, got {system}/{arch}.")
    return {"Darwin": "macos", "Windows": "windows"}.get(system, "linux"), arch


def verify_windows_imports(stage, toolchain):
    name = "bin/llvm-readobj.exe" if (toolchain / "bin/llvm-readobj.exe").is_file() else "bin/llvm-readobj"
    reader = regular_file(toolchain, name)
    binaries = {path.name.lower(): path for path in stage.iterdir()
                if path.is_file() and path.suffix.lower() in (".exe", ".dll")}
    imports = {}
    for name, path in sorted(binaries.items()):
        # --coff-imports reports both Import and DelayImport records.
        report = run([str(reader), "--coff-imports", str(path)]).decode().replace("\r\n", "\n")
        if "Format: COFF-x86-64" not in report:
            raise PackageError(f"Expected an x86_64 PE binary: {path}")
        dependencies = sorted(set(re.findall(r"^  Name: (\S+)$", report, re.MULTILINE)))
        for dependency in dependencies:
            key = dependency.lower()
            if (key not in binaries and key not in WINDOWS_SYSTEM_DLLS
                    and not key.startswith(("api-ms-win-", "ext-ms-win-"))):
                raise PackageError(f"Windows package is missing {dependency}, imported by {path.name}")
        imports[path.name] = dependencies
    return imports


def verify_steamdeck_requirements(stage, binaries):
    """System libraries and symbol versions each binary needs, within SteamOS 3.7."""
    requirements = {}
    for name in binaries:
        report = run(["readelf", "--dynamic", "--version-info", "--wide", str(stage / name)]).decode()
        needed = sorted(set(re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", report)))
        versions = sorted(set(re.findall(r"^\s*0x[0-9a-f]+:\s+Name: (\S+)\s+Flags:", report, re.MULTILINE)))
        if not needed or not versions:
            raise PackageError(f"Could not read the dynamic dependencies of {name}.")
        for library in needed:
            if library not in STEAMDECK_SYSTEM_LIBRARIES:
                raise PackageError(f"{name} needs {library}, which SteamOS does not provide as a system library.")
        newest = {}
        for version in versions:
            match = re.fullmatch(r"([A-Z]+)_([0-9]+(?:\.[0-9]+)*)", version)
            if not match or match[1] not in STEAMDECK_SYMBOL_CEILINGS:
                raise PackageError(f"{name} needs unsupported symbol version {version}.")
            number = tuple(int(part) for part in match[2].split("."))
            if number > STEAMDECK_SYMBOL_CEILINGS[match[1]]:
                limit = ".".join(map(str, STEAMDECK_SYMBOL_CEILINGS[match[1]]))
                raise PackageError(f"{name} needs {version}, newer than SteamOS 3.7 ({match[1]} {limit}); "
                                   "build it with an older C library and compiler runtime.")
            if number > newest.get(match[1], ((),))[0]:
                newest[match[1]] = (number, version)
        requirements[name] = {"needed": needed, "newest": {k: v[1] for k, v in sorted(newest.items())}}
    return requirements


def is_notice(relative):
    upper = relative.name.upper()
    named = re.match(r"^(LICENSE|LICENCE|COPYING|NOTICE|COPYRIGHT)(?:[._-].*)?$", upper)
    in_licenses = any(part.upper() == "LICENSES" for part in relative.parts[:-1])
    code_suffixes = (".c", ".cc", ".cpp", ".h", ".hpp", ".rs", ".py", ".sh", ".json", ".gz", ".png")
    return (named or in_licenses or upper == "FTL.TXT") and not relative.name.lower().endswith(code_suffixes)


def notice_paths(root):
    """Only standalone license/notice texts, including nested third-party texts."""
    for directory, dirs, files in os.walk(root, followlinks=False):
        dirs[:] = sorted(d for d in dirs if d not in (".git", "target", "__pycache__")
                         and not (Path(directory) / d).is_symlink())
        for name in sorted(files):
            relative = (Path(directory) / name).relative_to(root)
            if is_notice(relative):
                yield relative


def cached_crate(path, name, version, checksum):
    data = regular_file(path.parent, path.name).read_bytes()
    if digest(data) != checksum:
        raise PackageError(f"Cached Rust archive disagrees with Cargo.lock: {name}-{version}")
    prefix = Path(f"{name}-{version}")
    notices = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        manifest = archive.getmember((prefix / "Cargo.toml").as_posix())
        if not manifest.isfile():
            raise PackageError(f"Unsafe cached Rust manifest: {name}-{version}")
        license_name = tomllib.loads(archive.extractfile(manifest).read().decode())["package"].get("license")
        for member in archive.getmembers():
            relative = Path(member.name).relative_to(prefix)
            if relative.is_absolute() or ".." in relative.parts:
                raise PackageError(f"Unsafe cached Rust notice path: {member.name}")
            if is_notice(relative):
                if not member.isfile() or relative.as_posix() in notices:
                    raise PackageError(f"Unsafe or duplicate cached Rust notice: {member.name}")
                notices[relative.as_posix()] = archive.extractfile(member).read()
    return license_name, notices


def cargo_packages(build, cache):
    features = []
    cmake_compression = cache.get("NOD_USE_CMAKE_COMPRESSION", "OFF").upper() in ("ON", "TRUE", "1")
    for name in ("BZIP2", "LZMA", "ZLIB", "ZSTD"):
        if cache.get("NOD_COMPRESS_" + name, "ON").upper() in ("ON", "TRUE", "1"):
            features.append("compress-" + name.lower() + ("" if cmake_compression else "-static"))
    if cache.get("NOD_THREADING", "ON").upper() in ("ON", "TRUE", "1"):
        features.append("threading")
    target = cache.get("Rust_CARGO_TARGET_CACHED")
    if not target:
        raise PackageError("Missing configured Rust target for the locked nod dependency inventory.")
    cargo = cache.get("CORROSION_TOOLS_CARGO") or "cargo"
    # --color never: CI sets CARGO_TERM_COLOR=always, which would put escape
    # codes into the lines parsed below.
    command = [cargo, "tree", "--color", "never", "--package", "nod-ffi", "--locked", "--offline",
               "--manifest-path", str(regular_file(build, "prepared/nod/source/nod-ffi/Cargo.toml")),
               "--no-default-features", "--target", target, "--prefix", "none",
               "--format", "{p}", "--edges", "normal,build"]
    if features:
        command += ["--features", ",".join(features)]
    try:
        tree = run(command).decode()
    except PackageError as error:
        raise PackageError(f"Cannot collect locked Rust notices. Use the same CARGO_HOME as the build. {error}") from error
    selected = set()
    for line in tree.splitlines():
        match = re.fullmatch(r"([A-Za-z0-9_-]+) v([A-Za-z0-9.+_-]+)(?: \([^\n]*\))?(?: \(\*\))?", line)
        if not match:
            raise PackageError(f"Malformed Cargo tree package line: {line!r}")
        selected.add(match.groups())
    if not any(name == "nod-ffi" for name, _ in selected):
        raise PackageError("Cargo tree did not return the nod-ffi package.")
    nod = build / "prepared/nod/source"
    lock = tomllib.loads(regular_file(nod, "Cargo.lock").read_text())
    workspace = tomllib.loads(regular_file(nod, "Cargo.toml").read_text())["workspace"]
    cargo_home = Path(os.environ.get("CARGO_HOME", Path.home() / ".cargo")).resolve()
    packages = []
    for name, version in sorted(selected):
        entries = [p for p in lock["package"] if (p["name"], p["version"]) == (name, version)]
        if len(entries) != 1:
            raise PackageError(f"Ambiguous or absent locked Rust package: {name}-{version}")
        entry = entries[0]
        if entry.get("source"):
            if not entry["source"].startswith("registry+"):
                raise PackageError(f"Unsupported Rust source: {entry['source']}")
            archives = [path for path in (cargo_home / "registry/cache").glob(f"*/{name}-{version}.crate")
                        if digest(regular_file(path.parent, path.name).read_bytes()) == entry.get("checksum")]
            if len(archives) != 1:
                raise PackageError(f"Require one cached locked source for {name}-{version}; use the build's CARGO_HOME.")
            archive = archives[0]
            license_name, _ = cached_crate(archive, name, version, entry["checksum"])
            location = {"crate_path": str(archive)}
        else:
            if name not in ("nod", "nod-ffi"):
                raise PackageError(f"Unexpected local nod dependency: {name}")
            root = nod / name
            manifest_path = regular_file(root, "Cargo.toml")
            package = tomllib.loads(manifest_path.read_text())["package"]
            license_name = package.get("license")
            if isinstance(license_name, dict) and license_name.get("workspace"):
                license_name = workspace["package"].get("license")
            location = {"manifest_path": str(manifest_path)}
        packages.append({"name": name, "version": version, "source": entry.get("source"),
                         **location, "license": license_name})
    return packages, {"target": target, "features": features, "locked": True, "offline": True,
                      "edges": ["normal", "build"]}


def package_build(build, output, source, cargo_loader=cargo_packages, steamdeck=False):
    build, output, source = build.resolve(), output.resolve(), source.resolve()
    cache = cache_values(build)
    system, arch = target_platform(build)
    if steamdeck and (system, arch) != ("linux", "x86_64"):
        raise PackageError(f"A Steam Deck package needs a Linux x86_64 build, got {system}/{arch}.")
    version_body = regular_file(build, "generated/mscharged/build_version.h").read_text()
    match = re.search(r'const char\* version\s*=\s*"([A-Za-z0-9._+\-]+)"', version_body)
    if not match:
        raise PackageError("Missing or invalid embedded build version.")
    version = match[1]
    windows = system == "windows"
    executable_name = "mscharged.exe" if windows else "mscharged"
    executable = regular_file(build, executable_name)
    if not windows and not executable.stat().st_mode & 0o111:
        raise PackageError("mscharged is not executable; refusing an unusable package.")
    runtime_root = None
    graphics_root = None
    graphics_pin = None
    if windows:
        if not cache.get("LLVM_MINGW_ROOT"):
            raise PackageError("A Windows build needs LLVM_MINGW_ROOT for its runtime DLLs.")
        runtime_root = Path(cache["LLVM_MINGW_ROOT"]).resolve()
        if not cache.get("MSCHARGED_WINDOWS_GRAPHICS_DIR"):
            raise PackageError("A Windows build needs MSCHARGED_WINDOWS_GRAPHICS_DIR for its shader compiler DLLs.")
        graphics_root = Path(cache["MSCHARGED_WINDOWS_GRAPHICS_DIR"]).resolve()
        graphics_pin = json.loads(WINDOWS_GRAPHICS_PIN.read_text())
        if arch != graphics_pin["architecture"]:
            raise PackageError("Windows shader compiler architecture does not match the build.")
        try:
            verify_windows_graphics(graphics_root, graphics_pin)
        except ValueError as error:
            raise PackageError(str(error)) from error
    modules = []
    suffixes = (".dll",) if windows else (".so", ".dylib", ".bundle")
    for stem in MODULES:
        matches = [stem + suffix for suffix in suffixes if (build / (stem + suffix)).exists()]
        if len(matches) != 1:
            raise PackageError(f"Require exactly one loadable {stem} module, found {len(matches)}.")
        regular_file(build, matches[0])
        modules += matches
    # A stale or missing catalogue must fail packaging, not silently restore
    # first-use compilation stalls in a downloadable archive.
    catalog = regular_file(source, "assets/graphics/pipelines.json")
    pipeline_resource = regular_file(build, "assets/graphics/initial_pipeline_cache.db")
    try:
        verify_database(pipeline_resource, catalog_rows(catalog))
    except CatalogError as error:
        raise PackageError(str(error)) from error
    revision = run(["git", "-C", str(source), "rev-parse", "HEAD"]).decode().strip()
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise PackageError("Invalid packaging source Git revision.")
    packages, cargo_options = cargo_loader(build, cache)
    package_name = f"mscharged-{version}-{'steamdeck' if steamdeck else system}-{arch}"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".mscharged-package-", dir=output) as temporary:
        stage = Path(temporary) / package_name
        stage.mkdir()
        files = {}

        def put(relative, data, mode=0o644):
            relative = Path(relative)
            if relative.is_absolute() or ".." in relative.parts or relative.as_posix() in files:
                raise PackageError(f"Invalid or duplicate package path: {relative}")
            destination = stage / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            destination.chmod(mode)
            files[relative.as_posix()] = {"sha256": digest(data), "bytes": len(data), "mode": oct(mode)}

        def copy(root, relative, destination=None, expected=None):
            path = regular_file(root, relative)
            data = path.read_bytes()
            if expected and digest(data) != expected:
                raise PackageError(f"File differs from its recorded source: {path}")
            put(destination or relative, data, stat.S_IMODE(path.stat().st_mode))

        copy(build, executable_name)
        for module in modules:
            copy(build, module)
        if windows:
            for dll in WINDOWS_RUNTIME:
                copy(runtime_root / "x86_64-w64-mingw32/bin", dll)
            for notice in WINDOWS_RUNTIME_NOTICES:
                copy(runtime_root, notice, Path("LICENSES/toolchain/llvm-mingw") / Path(notice).name)
            for record in graphics_file_records(graphics_pin):
                relative = record["path"]
                # Package the DLLs actually used by this build. Reject stale,
                # missing, or modified copies instead of masking them at packaging.
                if relative.endswith(".dll"):
                    copy(build, relative, expected=record["sha256"])
                else:
                    copy(graphics_root, relative, expected=record["sha256"])
            copy(graphics_root, "windows-graphics.json", "LICENSES/windows-graphics.json")
        for asset in ASSETS:
            copy(build, Path("assets/launcher") / asset)
        copy(build, "assets/graphics/initial_pipeline_cache.db")
        copy(source, "assets/graphics/README.md")
        for public in PUBLIC_FILES:
            copy(source, public)

        prepared = []
        for dependency in DEPENDENCIES:
            root = build / "prepared" / dependency
            manifest_path = regular_file(root, "manifest.json")
            manifest_bytes = manifest_path.read_bytes()
            manifest = json.loads(manifest_bytes)
            inputs = manifest["inputs"]
            if inputs["dependency"] != dependency or not inputs["sources"]:
                raise PackageError(f"Invalid prepared provenance: {manifest_path}")
            prepared.append({"dependency": dependency, "manifest_sha256": digest(manifest_bytes),
                             "key": manifest["key"], "inputs": inputs})
            notice_root = Path("LICENSES/dependencies") / dependency
            if dependency == "mscharged-decomp":
                # Its prepared export contains only include/libs/src, so obtain
                # notices from the exact recorded Git object, not a moving checkout.
                pin = next(item["commit"] for item in inputs["sources"] if item["path"] == "")
                if not re.fullmatch(r"[0-9a-f]{40}", pin):
                    raise PackageError("Invalid decomp source pin.")
                git = ["git", "-C", str(source / "extern/mscharged-decomp")]
                paths = run(git + ["ls-tree", "-r", "--name-only", "-z", pin, "--", "LICENSE", "LICENSES"])
                notices = [Path(path.decode()) for path in paths.split(b"\0") if path]
                if Path("LICENSE") not in notices:
                    raise PackageError("The pinned decomp source has no root license notice.")
                for path in notices:
                    put(notice_root / path, run(git + ["show", pin + ":" + path.as_posix()]))
            else:
                notices = list(notice_paths(root / "source"))
                if not notices:
                    raise PackageError(f"No dependency license notices found: {dependency}")
                for path in notices:
                    recorded = manifest["content"].get(path.as_posix())
                    if not recorded:
                        raise PackageError(f"Unrecorded prepared notice: {dependency}/{path}")
                    copy(root / "source", path, notice_root / path, recorded["sha256"])
                if dependency == "freetype" and Path("docs/FTL.TXT") not in notices:
                    raise PackageError("Missing selected FreeType License docs/FTL.TXT.")

        nod = build / "prepared/nod/source"
        lock_bytes = regular_file(nod, "Cargo.lock").read_bytes()
        lock = tomllib.loads(lock_bytes.decode())
        locked = {(p["name"], p["version"], p.get("source")): p for p in lock["package"]}
        copy(nod, "Cargo.lock", "LICENSES/rust/Cargo.lock")
        rust = []
        for package in sorted(packages, key=lambda p: (p["name"], p["version"])):
            key = (package["name"], package["version"], package.get("source"))
            if key not in locked:
                raise PackageError(f"Rust package is absent from Cargo.lock: {key[:2]}")
            item = {"name": key[0], "version": key[1], "source": key[2],
                    "license": package.get("license"), "checksum": locked[key].get("checksum")}
            if key[2]:
                if not key[2].startswith("registry+") or not item["checksum"]:
                    raise PackageError(f"Unsupported Rust source; collect its exact notices explicitly: {key}")
                _, notices = cached_crate(Path(package["crate_path"]), key[0], key[1], item["checksum"])
                if not notices:
                    raise PackageError(f"No cached Rust license texts: {key[:2]}")
                destination = Path("LICENSES/rust") / f"{key[0]}-{key[1]}"
                for path, data in notices.items():
                    put(destination / path, data)
                item["notices"] = [(destination / path).as_posix() for path in sorted(notices)]
            elif not Path(package["manifest_path"]).resolve().parent.is_relative_to(nod):
                raise PackageError(f"Unexpected unpinned local Rust dependency: {key[:2]}")
            rust.append(item)

        run_command = "mscharged.exe" if windows else "./mscharged"
        runtime_note = ", the runtime DLLs" if windows else ""
        readme = f"""# mscharged build package

Build: {version} ({system}, {arch}).

Extract the entire directory, then start {executable_name} (the launcher), or
run the game directly from a terminal:

    {run_command} --disc /path/to/R4QE01.rvz --window

Keep the loadable modules{runtime_note} and assets beside the executable. Game data is not
included; players supply their own disc image (Mario Strikers Charged, USA
R4QE01 revision 1 or Europe R4QP01 revision 1 or 2, as ISO or RVZ). docs/INSTALL.md covers the first start,
docs/SETTINGS.md the settings and docs/TROUBLESHOOTING.md common problems.

Host prerequisites remain system components: on Linux, the C/C++ runtimes,
Vulkan loader/driver and desktop/input/audio services; on macOS, the system
C/C++ runtimes and Metal, Cocoa, QuartzCore and CoreAudio frameworks; on
Windows 10/11, the Universal C Runtime and a Vulkan or DirectX 12 driver. Select
the Windows graphics API in the launcher's Advanced page. The LLVM-MinGW C++
runtime and DirectX shader compiler DLLs are included on Windows (notices in
LICENSES/toolchain, LICENSES/dxc and LICENSES/windows-sdk). This
archive does not bundle system libraries and is not signed.

SOURCE-MANIFEST.json records the embedded build version, packaging source HEAD,
prepared pins/patches and payload hashes. The packaging HEAD can differ from the
compiled revision in the embedded version. LICENSES/dependencies contains the
prepared source notices (including build-time/nested components); LICENSES/rust
contains Cargo.lock and notices for the resolved nod-ffi graph. This conservative
notice inventory does not assert that every configured component is linked.
FreeType uses the FreeType License; Zstandard uses its BSD option. Source links
and any component-specific obligations remain in the accompanying notices.
Launcher artwork provenance and its unspecified redistribution license are
recorded in assets/launcher/README.md and LICENSES/README.md.
"""
        if steamdeck:
            readme += """
## Steam Deck

This package is the Linux x86_64 build checked against SteamOS 3.7 or newer:
it needs no newer system libraries than glibc 2.40 and the GCC 14 C++ runtime.
docs/STEAM_DECK.md has the full guide; in short:

1. In Desktop Mode, extract the directory to a folder you can write to, for
   example ~/Games/mscharged.
2. In Steam, choose Games > Add a Non-Steam Game to My Library, browse to
   mscharged in that folder and add it. Leave Compatibility unchecked.
3. In Gaming Mode, apply the Gamepad controller template to it and start it.
   Choose your disc image on the launcher's Game page, set player 1 to
   gamepad1 on the Controls page and press Play.
"""
        put("PACKAGE-README.md", readme.encode())
        manifest = {"format": 1, "version": version, "platform": "steamdeck" if steamdeck else system,
                    "architecture": arch,
                    "packaging_source_commit": revision, "modules": modules, "prepared": prepared,
                    "cargo": {"options": cargo_options, "lock_sha256": digest(lock_bytes), "packages": rust},
                    "files": files}
        if steamdeck:
            manifest["steamdeck_requirements"] = verify_steamdeck_requirements(stage, [executable_name] + modules)
        if windows:
            manifest["windows_graphics"] = graphics_pin
            manifest["windows_imports"] = verify_windows_imports(stage, runtime_root)
        # The manifest describes the payload before itself, avoiding a self-hash.
        put("SOURCE-MANIFEST.json", (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode())
        extension = ".zip" if windows else ".tar.gz"
        temporary_archive = Path(temporary) / ("package" + extension)
        if windows:
            # Windows users expect a zip; Explorer opens it without extra tools.
            # Files sit at the zip's root: Extract All already creates a folder
            # named after the zip, which a top-level folder would nest again.
            with zipfile.ZipFile(temporary_archive, "w", zipfile.ZIP_DEFLATED) as archive:
                for path in sorted(stage.rglob("*")):
                    if path.is_file():
                        archive.write(path, path.relative_to(stage).as_posix())
        else:
            with tarfile.open(temporary_archive, "w:gz") as archive:
                for path in [stage] + sorted(stage.rglob("*")):
                    info = archive.gettarinfo(str(path), str(Path(package_name) / path.relative_to(stage)))
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    if path.is_file():
                        with path.open("rb") as stream:
                            archive.addfile(info, stream)
                    else:
                        archive.addfile(info)
        destination = output / (package_name + extension)
        os.replace(temporary_archive, destination)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--source-dir", type=Path, default=Path(__file__).resolve().parents[1],
                        help="Port checkout (defaults to the script's repository).")
    parser.add_argument("--steamdeck", action="store_true",
                        help="Package a Linux x86_64 build for SteamOS 3.7+ and check its system requirements.")
    args = parser.parse_args()
    try:
        archive = package_build(args.build_dir, args.output_dir, args.source_dir, steamdeck=args.steamdeck)
    except (OSError, ValueError, KeyError, StopIteration) as error:
        print(f"Packaging failed: {error}", file=sys.stderr)
        return 1
    print(archive)
    return 0


if __name__ == "__main__":
    sys.exit(main())
