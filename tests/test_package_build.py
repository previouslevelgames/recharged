"""Synthetic package allowlist, provenance and extraction checks; no game data."""

from contextlib import closing
import hashlib
import importlib.util
import io
import json
import pathlib
import shutil
import sys
import tarfile
import tempfile
import unittest
import zipfile
from unittest import mock

SOURCE = pathlib.Path(__file__).resolve().parents[1] / "tools/package_build.py"
sys.path.insert(0, str(SOURCE.parent))
SPEC = importlib.util.spec_from_file_location("package_build", SOURCE)
PACKAGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGER)


class PackageBuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.source = self.root / "source checkout with spaces"
        self.build = self.root / "completed build with spaces"
        self.output = self.root / "package artifacts with spaces"
        self.pin = "1" * 40
        self.checksum = "2" * 64
        self.write(self.build, "recharged", b"synthetic executable", 0o755)
        for stem in PACKAGER.MODULES:
            self.write(self.build, stem + ".so", b"synthetic module", 0o755)
        for asset in PACKAGER.ASSETS:
            self.write(self.build, "assets/launcher/" + asset, b"synthetic asset")
        from pipeline_catalog import ABI, generate
        catalog = {"format": 1, "schema": 1, "config_abi": ABI,
                   "pipelines": [{"type": 0, "hash": "0000000000000001", "version": 5,
                                  "first_frame": 0, "config": "0500000001010100",
                                  "sha256": self.hash(bytes.fromhex("0500000001010100"))}]}
        path = self.write(self.source, "assets/graphics/pipelines.json", json.dumps(catalog).encode())
        generate(self.build / "assets/graphics/initial_pipeline_cache.db", PACKAGER.catalog_rows(path))
        self.write(self.source, "assets/graphics/README.md", b"pipeline descriptions only")
        self.write(self.source, "assets/gameplay/robot_goalie.map", b"RGM1 synthetic map")
        self.write(self.source, "assets/gameplay/README.md", b"texture coordinates only")
        for public in PACKAGER.PUBLIC_FILES:
            self.write(self.source, public, b"public text")
        self.write(self.build, "generated/mscharged/build_version.h",
                   b'inline constexpr const char* version = "1.0.0+g123456789abc";\n')
        self.write(self.build, "CMakeCache.txt",
                   b"MSCHARGED_BUILD_ORIGINAL_FRONTEND_DIAGNOSTIC:BOOL=ON\n"
                   b"Rust_CARGO_TARGET_CACHED:INTERNAL=x86_64-unknown-linux-gnu\n")
        self.write(self.build, "CMakeFiles/4.4.4/CMakeSystem.cmake",
                   b'set(CMAKE_SYSTEM_NAME "Linux")\nset(CMAKE_SYSTEM_PROCESSOR "x86_64")\n')
        for dependency in PACKAGER.DEPENDENCIES:
            content = {}
            if dependency != "mscharged-decomp":
                self.dependency_notice(dependency, "LICENSE", b"root license", content)
            sources = [{"path": "", "commit": self.pin}]
            if dependency == "dawn":
                sources += [{"path": "third_party/spirv-tools/src", "commit": "3" * 40}]
                self.dependency_notice(dependency, "third_party/spirv-tools/src/LICENSE",
                                       b"nested license", content)
            if dependency == "freetype":
                self.dependency_notice(dependency, "docs/FTL.TXT", b"FTL selected", content)
            self.write(self.build, f"prepared/{dependency}/manifest.json", json.dumps({
                "key": "4" * 64, "content": content,
                "inputs": {"dependency": dependency, "sources": sources,
                           "patches": [{"name": "0001.patch", "sha256": "5" * 64}]},
            }).encode())
        self.write(self.build, "prepared/nod/source/Cargo.lock", f'''version = 4
[[package]]
name = "nod-ffi"
version = "1.0.0"
[[package]]
name = "codec"
version = "2.0.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "{self.checksum}"
'''.encode())
        crate = self.root / "cargo-cache/codec-2.0.0"
        self.write(crate, "Cargo.toml", b'[package]\nlicense="MIT"\n')
        self.write(crate, "LICENSE-MIT", b"cached crate copyright and license")
        self.write(crate, "vendor/COPYING", b"vendored codec license")
        self.crate_archive = self.root / "cargo-cache/registry/cache/index/codec-2.0.0.crate"
        self.cache_crate(crate)
        self.packages = [
            {"id": "nod-ffi", "name": "nod-ffi", "version": "1.0.0", "source": None,
             "manifest_path": str(self.build / "prepared/nod/source/nod-ffi/Cargo.toml"), "license": "MIT"},
            {"id": "codec", "name": "codec", "version": "2.0.0",
             "source": "registry+https://github.com/rust-lang/crates.io-index",
             "crate_path": str(self.crate_archive), "license": "MIT"},
        ]
        self.run_mock = mock.patch.object(PACKAGER, "run", side_effect=self.git_output)
        self.run_mock.start()
        self.addCleanup(self.run_mock.stop)

    @staticmethod
    def hash(data):
        return hashlib.sha256(data).hexdigest()

    @staticmethod
    def write(root, relative, data, mode=0o644):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)
        return path

    def dependency_notice(self, dependency, relative, data, content):
        self.write(self.build, f"prepared/{dependency}/source/{relative}", data)
        content[relative] = {"sha256": self.hash(data), "executable": False}

    def cache_crate(self, source):
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode="w:gz") as archive:
            for path in sorted(source.rglob("*")):
                if path.is_file():
                    archive.add(path, arcname=str(pathlib.Path("codec-2.0.0") / path.relative_to(source)))
        previous = self.checksum
        self.checksum = self.hash(data.getvalue())
        self.write(self.crate_archive.parent, self.crate_archive.name, data.getvalue())
        lock = self.build / "prepared/nod/source/Cargo.lock"
        lock.write_text(lock.read_text().replace(previous, self.checksum))

    @staticmethod
    def readelf_report(needed, versions):
        # The parts of `readelf --dynamic --version-info --wide` the packager reads.
        lines = [f" 0x0000000000000001 (NEEDED)             Shared library: [{library}]" for library in needed]
        lines += ["", "Version needs section '.gnu.version_r' contains 1 entry:",
                  "  000000: Version: 1  File: libc.so.6  Cnt: 1"]
        lines += [f"  0x{16 * (n + 1):04x}:   Name: {version}  Flags: none  Version: {n + 2}"
                  for n, version in enumerate(versions)]
        return ("\n".join(lines) + "\n").encode()

    def git_output(self, command):
        if command[0] == "readelf":
            name = pathlib.Path(command[-1]).name
            default = (["libstdc++.so.6", "libm.so.6", "libgcc_s.so.1", "libc.so.6"],
                       ["GLIBC_2.2.5", "GLIBC_2.39", "GLIBCXX_3.4.33", "CXXABI_1.3.15", "GCC_3.0"])
            return self.readelf_report(*getattr(self, "elf", {}).get(name, default))
        if "--coff-imports" in command:
            name = pathlib.Path(command[-1]).name
            imports = self.windows_imports.get(name, ["KERNEL32.dll"])
            return ("Format: COFF-x86-64\n" + "".join(f"Import {{\n  Name: {dll}\n}}\n" for dll in imports)).encode()
        self.assertEqual(command[0], "git")
        if "rev-parse" in command:
            return ("6" * 40 + "\n").encode()
        if "ls-tree" in command:
            self.assertIn(self.pin, command)
            return b"LICENSE\0LICENSES/ODE-BSD.txt\0"
        self.assertEqual(command[-2], "show")
        self.assertTrue(command[-1].startswith(self.pin + ":"))
        return b"pinned decomp license"

    def package(self, steamdeck=False):
        return PACKAGER.package_build(self.build, self.output, self.source,
                                      lambda build, cache: (self.packages, {"locked": True, "offline": True}),
                                      steamdeck=steamdeck)

    def test_allowlist_archive_manifest_and_executable_mode(self):
        for root, path in ((self.source, "recharged.ini"), (self.source, "game/R4QE01.rvz"),
                           (self.source, ".private/secret.md"), (self.build, "cache/driver.bin"),
                           (self.build, "recharged_game_us.map"),
                           (self.build, "mscharged_original_loading_module.so"),
                           (self.build, "assets/launcher/extra-private.txt")):
            self.write(root, path, b"MUST NOT BE PACKAGED")
        archive_path = self.package()
        self.assertTrue(archive_path.name.endswith("-linux-x86_64.tar.gz"))
        with tarfile.open(archive_path) as archive:
            members = archive.getmembers()
            self.assertTrue(all(not member.issym() and not member.islnk() for member in members))
            manifest_member = next(member for member in members if member.name.endswith("/SOURCE-MANIFEST.json"))
            manifest = json.load(archive.extractfile(manifest_member))
            self.assertEqual(manifest["version"], "1.0.0+g123456789abc")
            self.assertEqual(manifest["packaging_source_commit"], "6" * 40)
            self.assertEqual(len(manifest["modules"]), 2)
            self.assertEqual(len(manifest["prepared"]), len(PACKAGER.DEPENDENCIES))
            self.assertEqual(manifest["cargo"]["packages"][0]["name"], "codec")
            self.assertEqual(len(manifest["cargo"]["packages"]), 2)
            self.assertIn("LICENSES/dependencies/dawn/third_party/spirv-tools/src/LICENSE", manifest["files"])
            self.assertIn("LICENSES/dependencies/freetype/docs/FTL.TXT", manifest["files"])
            self.assertIn("LICENSES/dependencies/mscharged-decomp/LICENSES/ODE-BSD.txt", manifest["files"])
            self.assertIn("LICENSES/rust/codec-2.0.0/vendor/COPYING", manifest["files"])
            for name, item in manifest["files"].items():
                member = archive.getmember(manifest_member.name.rsplit("/", 1)[0] + "/" + name)
                self.assertEqual(self.hash(archive.extractfile(member).read()), item["sha256"])
                self.assertEqual(member.mode, int(item["mode"], 8))
                self.assertEqual(member.uid, 0)
            self.assertEqual(manifest["files"]["recharged"]["mode"], "0o755")
            self.assertIn("assets/graphics/initial_pipeline_cache.db", manifest["files"])
            self.assertIn("assets/graphics/README.md", manifest["files"])
            self.assertIn("assets/gameplay/robot_goalie.map", manifest["files"])
            self.assertIn("assets/gameplay/README.md", manifest["files"])
            self.assertFalse(any("MUST NOT BE PACKAGED" in archive.extractfile(member).read().decode(errors="ignore")
                                 for member in members if member.isfile()))
            extraction = self.root / "extracted"
            archive.extractall(extraction, filter="data")
            executable = next(extraction.glob("*/recharged"))
            self.assertTrue(executable.stat().st_mode & 0o111)

    def test_pipeline_resource_is_required_and_matches_catalogue(self):
        path = self.build / "assets/graphics/initial_pipeline_cache.db"
        original = path.read_bytes()
        path.unlink()
        with self.assertRaisesRegex(PACKAGER.PackageError, "Missing or unsafe"):
            self.package()
        self.write(path.parent, path.name, original)
        import sqlite3
        with closing(sqlite3.connect(path)) as db:
            db.execute("UPDATE pipeline_cache SET first_frame_used=42")
            db.commit()
        with self.assertRaisesRegex(PACKAGER.PackageError, "differs from its reviewed"):
            self.package()
        self.assertFalse(self.output.exists())

    def test_missing_module_fails_without_archive(self):
        (self.build / (PACKAGER.MODULES[1] + ".so")).unlink()
        with self.assertRaisesRegex(PACKAGER.PackageError, "Require exactly one"):
            self.package()
        self.assertFalse(self.output.exists())

    def test_payload_survives_relocation_with_spaces(self):
        archive_path = self.package()
        extracted = self.root / "first extraction with spaces"
        with tarfile.open(archive_path) as archive:
            archive.extractall(extracted, filter="data")
        package = next(extracted.iterdir())
        relocated = self.root / "moved runtime with spaces"
        package.rename(relocated)
        # Retain only the synthetic distributable. This proves archive paths,
        # bytes and modes; it does not execute or qualify a native runtime.
        shutil.rmtree(self.build)
        shutil.rmtree(self.source)
        manifest = json.loads((relocated / "SOURCE-MANIFEST.json").read_text())
        self.assertEqual(manifest["modules"], ["recharged_game_us.so", "recharged_game_eu.so"])
        self.assertFalse((relocated / "prepared").exists())
        self.assertFalse((relocated / "mscharged_original_loading_module.so").exists())
        for relative, recorded in manifest["files"].items():
            path = relocated / relative
            self.assertTrue(path.is_file() and not path.is_symlink())
            self.assertEqual(self.hash(path.read_bytes()), recorded["sha256"])
            self.assertEqual(path.stat().st_mode & 0o777, int(recorded["mode"], 8))

    def test_ambiguous_module_fails(self):
        self.write(self.build, PACKAGER.MODULES[0] + ".dylib", b"different module")
        with self.assertRaisesRegex(PACKAGER.PackageError, "found 2"):
            self.package()

    def test_missing_asset_or_public_notice_fails(self):
        for root, path in ((self.build, "assets/launcher/Roboto-Medium.ttf"),
                           (self.source, "LICENSES/Apache-2.0.txt")):
            original = (root / path).read_bytes()
            (root / path).unlink()
            with self.subTest(path=path), self.assertRaisesRegex(PACKAGER.PackageError, "Missing or unsafe"):
                self.package()
            self.write(root, path, original)
        self.assertEqual(list(self.output.glob("*.tar.gz")), [])

    def test_symlink_does_not_copy_external_private_data(self):
        path = self.build / "assets/launcher/README.md"
        path.unlink()
        secret = self.write(self.root, "secret.ini", b"private")
        path.symlink_to(secret)
        with self.assertRaisesRegex(PACKAGER.PackageError, "Missing or unsafe"):
            self.package()

    def test_nonexecutable_and_launcher_only_builds_fail(self):
        (self.build / "recharged").chmod(0o644)
        with self.assertRaisesRegex(PACKAGER.PackageError, "not executable"):
            self.package()
        (self.build / "recharged").chmod(0o755)
        self.write(self.build, "CMakeCache.txt", b"MSCHARGED_BUILD_ORIGINAL_FRONTEND_DIAGNOSTIC:BOOL=OFF\n")
        with self.assertRaisesRegex(PACKAGER.PackageError, "not only the launcher"):
            self.package()

    def use_macos_build(self):
        self.write(self.build, "CMakeFiles/4.4.4/CMakeSystem.cmake",
                   b'set(CMAKE_SYSTEM_NAME "Darwin")\nset(CMAKE_SYSTEM_PROCESSOR "arm64")\n')
        for stem in PACKAGER.MODULES:
            (self.build / (stem + ".so")).rename(self.build / (stem + ".dylib"))
        for item in PACKAGER.MACOS_APP_FILES:
            self.write(self.build, "Recharged.app/" + item, b"synthetic bundle " + item.encode(),
                       0o755 if item.endswith("/recharged") else 0o644)

    def test_macos_uses_configured_target_and_preserves_module_name(self):
        self.use_macos_build()
        path = self.package()
        self.assertTrue(path.name.endswith("-macos-arm64.tar.gz"))
        with tarfile.open(path) as archive:
            member = next(item for item in archive if item.name.endswith("/SOURCE-MANIFEST.json"))
            manifest = json.load(archive.extractfile(member))
            root = member.name.rsplit("/", 1)[0]
            script = archive.extractfile(root + "/recharged").read()
        self.assertEqual((manifest["platform"], manifest["architecture"]), ("macos", "arm64"))
        self.assertEqual(manifest["modules"], [stem + ".dylib" for stem in PACKAGER.MODULES])
        # The app as built, and a start script instead of the plain program.
        for item in PACKAGER.MACOS_APP_FILES:
            self.assertIn("Recharged.app/" + item, manifest["files"])
        self.assertEqual(manifest["files"]["Recharged.app/Contents/MacOS/recharged"]["mode"], "0o755")
        self.assertEqual(manifest["files"]["recharged"]["mode"], "0o755")
        self.assertEqual(script, PACKAGER.MACOS_START_SCRIPT)

    def test_macos_requires_the_app_as_built(self):
        self.use_macos_build()
        (self.build / "Recharged.app/Contents/_CodeSignature/CodeResources").unlink()
        with self.assertRaisesRegex(PACKAGER.PackageError, "CodeResources"):
            self.package()
        self.write(self.build, "Recharged.app/Contents/_CodeSignature/CodeResources", b"signature")
        self.write(self.build, "Recharged.app/Contents/Resources/stale.txt", b"left over")
        with self.assertRaisesRegex(PACKAGER.PackageError, "unexpected files: Contents/Resources/stale.txt"):
            self.package()

    def test_steamdeck_package_records_requirements_within_steamos(self):
        path = self.package(steamdeck=True)
        self.assertTrue(path.name.endswith("-steamdeck-x86_64.tar.gz"))
        with tarfile.open(path) as archive:
            member = next(item for item in archive if item.name.endswith("/SOURCE-MANIFEST.json"))
            manifest = json.load(archive.extractfile(member))
            readme = archive.extractfile(next(item for item in archive
                                              if item.name.endswith("/PACKAGE-README.md"))).read().decode()
        self.assertEqual((manifest["platform"], manifest["architecture"]), ("steamdeck", "x86_64"))
        requirements = manifest["steamdeck_requirements"]
        self.assertEqual(sorted(requirements), sorted(["recharged"] + [m + ".so" for m in PACKAGER.MODULES]))
        self.assertEqual(requirements["recharged"]["newest"],
                         {"CXXABI": "CXXABI_1.3.15", "GCC": "GCC_3.0", "GLIBC": "GLIBC_2.39",
                          "GLIBCXX": "GLIBCXX_3.4.33"})
        self.assertIn("Add a Non-Steam Game", readme)
        self.assertNotIn("steamdeck_requirements", json.loads(self.manifest_of(self.package())))

    def manifest_of(self, path):
        with tarfile.open(path) as archive:
            member = next(item for item in archive if item.name.endswith("/SOURCE-MANIFEST.json"))
            return archive.extractfile(member).read()

    def test_steamdeck_rejects_newer_or_unknown_system_requirements(self):
        cases = {
            "GLIBC_2.43": (["libc.so.6"], ["GLIBC_2.43"]),
            "GLIBCXX_3.4.34": (["libstdc++.so.6"], ["GLIBC_2.2.5", "GLIBCXX_3.4.34"]),
            "CXXABI_1.3.16": (["libstdc++.so.6"], ["CXXABI_1.3.16"]),
            "GLIBC_PRIVATE": (["libc.so.6"], ["GLIBC_PRIVATE"]),
            "libSDL3.so.0": (["libSDL3.so.0", "libc.so.6"], ["GLIBC_2.2.5"]),
            "dynamic dependencies": ([], []),
        }
        for message, report in cases.items():
            with self.subTest(message=message):
                self.elf = {PACKAGER.MODULES[0] + ".so": report}
                with self.assertRaisesRegex(PACKAGER.PackageError, message.replace(".", r"\.")):
                    self.package(steamdeck=True)
        self.assertEqual(list(self.output.glob("*.tar.gz")), [])

    def test_steamdeck_requires_a_linux_x86_64_build(self):
        self.write(self.build, "CMakeFiles/4.4.4/CMakeSystem.cmake",
                   b'set(CMAKE_SYSTEM_NAME "Linux")\nset(CMAKE_SYSTEM_PROCESSOR "aarch64")\n')
        with self.assertRaisesRegex(PACKAGER.PackageError, "Linux x86_64 build"):
            self.package(steamdeck=True)

    def windows_build(self):
        self.write(self.build, "CMakeFiles/4.4.4/CMakeSystem.cmake",
                   b'set(CMAKE_SYSTEM_NAME "Windows")\nset(CMAKE_SYSTEM_PROCESSOR "x86_64")\n')
        (self.build / "recharged").rename(self.build / "recharged.exe")
        for stem in PACKAGER.MODULES:
            (self.build / (stem + ".so")).rename(self.build / (stem + ".dll"))
        toolchain = self.root / "llvm mingw"
        self.write(toolchain, "bin/llvm-readobj", b"fixture tool")
        self.windows_imports = {"recharged.exe": ["libc++.dll"],
                                "libc++.dll": ["libunwind.dll"],
                                "libunwind.dll": ["libwinpthread-1.dll", "api-ms-win-crt-runtime-l1-1-0.dll"]}
        for dll in PACKAGER.WINDOWS_RUNTIME:
            self.write(toolchain, "x86_64-w64-mingw32/bin/" + dll, b"runtime " + dll.encode())
        for notice in PACKAGER.WINDOWS_RUNTIME_NOTICES:
            self.write(toolchain, notice, b"notice")
        cache = (self.build / "CMakeCache.txt").read_bytes()
        self.write(self.build, "CMakeCache.txt", cache + f"LLVM_MINGW_ROOT:PATH={toolchain}\n".encode())
        graphics = self.root / "windows graphics"
        pin = json.loads(PACKAGER.WINDOWS_GRAPHICS_PIN.read_text())
        for record in PACKAGER.graphics_file_records(pin):
            data = ("synthetic " + record["path"]).encode()
            record["sha256"] = hashlib.sha256(data).hexdigest()
            self.write(graphics, record["path"], data)
            if record["path"].endswith(".dll"):
                self.write(self.build, record["path"], data)
        self.write(graphics, "windows-graphics.json", json.dumps(pin).encode())
        pin_path = self.root / "graphics pin.json"
        pin_path.write_text(json.dumps(pin))
        patch = mock.patch.object(PACKAGER, "WINDOWS_GRAPHICS_PIN", pin_path)
        patch.start()
        self.addCleanup(patch.stop)
        with (self.build / "CMakeCache.txt").open("a") as stream:
            stream.write(f"MSCHARGED_WINDOWS_GRAPHICS_DIR:PATH={graphics}\n")
        return graphics, pin

    def test_windows_packages_exe_dlls_runtime_and_notices_as_zip(self):
        _, pin = self.windows_build()
        self.windows_imports["recharged.exe"].append("pdh.dll")
        path = self.package()
        self.assertTrue(path.name.endswith("-windows-x86_64.zip"))
        with zipfile.ZipFile(path) as archive:
            names = set(archive.namelist())
            manifest = json.loads(next(archive.read(n) for n in archive.namelist() if n.endswith("SOURCE-MANIFEST.json")))
        self.assertIn("recharged.exe", names)  # at the zip's root, no nested folder
        self.assertTrue(set(PACKAGER.WINDOWS_RUNTIME) <= names)
        self.assertIn("LICENSES/toolchain/llvm-mingw/COPYING.winpthreads.txt", names)
        self.assertIn("assets/launcher/aurora.png", names)
        self.assertEqual(manifest["modules"], [stem + ".dll" for stem in PACKAGER.MODULES])
        self.assertEqual(manifest["platform"], "windows")
        self.assertEqual(manifest["windows_graphics"], pin)
        self.assertEqual(manifest["windows_imports"]["recharged.exe"], ["libc++.dll", "pdh.dll"])
        self.assertNotIn("pdh.dll", names)  # Performance counters use the Windows OS component.
        for record in PACKAGER.graphics_file_records(pin):
            self.assertIn(record["path"], names)
            self.assertEqual(manifest["files"][record["path"]]["sha256"], record["sha256"])

    def test_windows_rejects_missing_shader_compiler(self):
        self.windows_build()
        (self.build / "dxil.dll").unlink()
        with self.assertRaisesRegex(PACKAGER.PackageError, "Missing.*dxil.dll"):
            self.package()

    def test_windows_rejects_stale_shader_compiler(self):
        self.windows_build()
        (self.build / "dxcompiler.dll").write_bytes(b"wrong version")
        with self.assertRaisesRegex(PACKAGER.PackageError, "differs.*dxcompiler.dll"):
            self.package()

    def test_windows_rejects_missing_compiler_notice(self):
        graphics, _ = self.windows_build()
        (graphics / "LICENSES/dxc/LICENSE-MS.txt").unlink()
        with self.assertRaisesRegex(PACKAGER.PackageError, "Missing or modified"):
            self.package()

    def test_windows_rejects_missing_transitive_runtime(self):
        self.windows_build()
        self.windows_imports["dxcompiler.dll"] = ["VCRUNTIME140.dll"]
        with self.assertRaisesRegex(PACKAGER.PackageError, "missing VCRUNTIME140.dll, imported by dxcompiler.dll"):
            self.package()

    def test_windows_without_toolchain_root_fails(self):
        self.write(self.build, "CMakeFiles/4.4.4/CMakeSystem.cmake",
                   b'set(CMAKE_SYSTEM_NAME "Windows")\nset(CMAKE_SYSTEM_PROCESSOR "x86_64")\n')
        (self.build / "recharged").rename(self.build / "recharged.exe")
        with self.assertRaisesRegex(PACKAGER.PackageError, "LLVM_MINGW_ROOT"):
            self.package()

    def test_changed_prepared_and_cached_notices_fail(self):
        prepared = self.build / "prepared/dawn/source/LICENSE"
        prepared.write_bytes(b"changed")
        with self.assertRaisesRegex(PACKAGER.PackageError, "differs from its recorded source"):
            self.package()
        prepared.write_bytes(b"root license")
        self.crate_archive.write_bytes(self.crate_archive.read_bytes() + b"changed")
        with self.assertRaisesRegex(PACKAGER.PackageError, "disagrees with Cargo.lock"):
            self.package()

    def test_missing_ftl_and_missing_cargo_notices_fail(self):
        ftl = self.build / "prepared/freetype/source/docs/FTL.TXT"
        ftl.unlink()
        with self.assertRaisesRegex(PACKAGER.PackageError, "Missing selected FreeType"):
            self.package()
        self.write(self.build, "prepared/freetype/source/docs/FTL.TXT", b"FTL selected")
        crate = self.root / "cargo-cache/codec-2.0.0"
        (crate / "LICENSE-MIT").unlink()
        (crate / "vendor/COPYING").unlink()
        self.cache_crate(crate)
        with self.assertRaisesRegex(PACKAGER.PackageError, "No cached Rust license"):
            self.package()

    def test_cargo_command_is_locked_offline_and_matches_configured_features(self):
        self.write(self.build, "prepared/nod/source/Cargo.toml", b'[workspace]\n[workspace.package]\nlicense="MIT"\n')
        self.write(self.build, "prepared/nod/source/nod-ffi/Cargo.toml", b'[package]\nlicense.workspace=true\n')
        tree = b"nod-ffi v1.0.0 (/source/nod-ffi)\ncodec v2.0.0\ncodec v2.0.0 (*)\n"
        with mock.patch.dict(PACKAGER.os.environ, {"CARGO_HOME": str(self.root / "cargo-cache")}), \
                mock.patch.object(PACKAGER, "run", return_value=tree) as run:
            packages, options = PACKAGER.cargo_packages(self.build, {
                "Rust_CARGO_TARGET_CACHED": "aarch64-apple-darwin", "NOD_COMPRESS_ZSTD": "OFF"})
            command = run.call_args.args[0]
            for argument in ("--locked", "--offline", "--no-default-features", "--target", "normal,build"):
                self.assertIn(argument, command)
            self.assertIn("aarch64-apple-darwin", command)
            self.assertEqual(options["features"], ["compress-bzip2-static", "compress-lzma-static",
                                                   "compress-zlib-static", "threading"])
            self.assertEqual(len(packages), 2)
            self.assertEqual(packages[1]["license"], "MIT")


if __name__ == "__main__":
    unittest.main()
