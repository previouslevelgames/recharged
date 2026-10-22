#!/usr/bin/env python3
"""Checks tools/link_static_game_image.py on a small module and program.

Usage: test_static_game_image.py CXX LD OBJCOPY NM READELF
"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "link_static_game_image.py"
CXX, LD, OBJCOPY, NM, READELF = (sys.argv[1:6] if len(sys.argv) >= 6 else
                                 ("c++", "ld", "objcopy", "nm", "readelf"))

MODULE = r"""
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <vector>
static char arena[1 << 16];
static unsigned used;
void* operator new(std::size_t n) { void* p = arena + used; used += (n + 15) & ~15u; return p; }
void operator delete(void*) noexcept {}
void operator delete(void*, std::size_t) noexcept {}
static int order[4], constructed;
struct Late { Late() { order[constructed++] = 2; } } late;
__attribute__((constructor(101))) static void Early() { order[constructed++] = 1; }
thread_local int t_value = 7;
extern "C" int main() { return 99; }
extern "C" int charged_entry() {
    std::vector<int> v;
    for (int i = 0; i < 10; ++i) v.push_back(i);
    return constructed == 2 && order[0] == 1 && order[1] == 2 && t_value == 7 && used == 144 ? v[9] : -1;
}
extern "C" void charged_throw() { throw std::runtime_error("from the image"); }
extern "C" int charged_unused_helper() { return 1; }
"""

PROGRAM = r"""
#include <cstdio>
#include <stdexcept>
#include <vector>
extern "C" int charged_image_test__charged_entry();
extern "C" void charged_image_test__charged_throw();
extern "C" char charged_image_test__charged_absent[] __attribute__((weak));
extern "C" void (*const __start_charged_image_test_init[])();
extern "C" void (*const __stop_charged_image_test_init[])();
int main() {
    std::vector<int> host(1000, 1);  // the program's operator new, not the image's
    for (auto* f = __start_charged_image_test_init; f != __stop_charged_image_test_init; ++f) (*f)();
    if (charged_image_test__charged_entry() != 9) return 1;
    if (charged_image_test__charged_absent) return 2;
    try { charged_image_test__charged_throw(); } catch (const std::exception&) { return host.size() == 1000 ? 0 : 3; }
    return 4;
}
"""


def run(*command, cwd=None):
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True)


class StaticGameImageTest(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix="static-game-image-test-"))
        (self.work / "module.cpp").write_text(MODULE)
        (self.work / "program.cpp").write_text(PROGRAM)
        (self.work / "exports.map").write_text(
            "{ global: charged_entry; charged_throw; charged_absent; local: *; };\n")
        (self.work / "providers.cpp").write_text("CHARGED_MISSING_PROVIDER(not_in_the_image)\n")
        flags = ["-O2", "-fPIC", "-ffunction-sections", "-fdata-sections", "-fvisibility=hidden"]
        result = run(CXX, *flags, "-c", "module.cpp", "-o", "module.o", cwd=self.work)
        self.assertEqual(result.returncode, 0, result.stderr)

    def tearDown(self):
        subprocess.run(["rm", "-rf", str(self.work)])

    def link(self, providers="providers.cpp", output="image.o"):
        return run(sys.executable, "-B", str(TOOL), "--ld", LD, "--objcopy", OBJCOPY, "--nm", NM,
                   "--readelf", READELF, "--image", "test", "--providers", providers, "--",
                   CXX, "-fPIC", "-shared", "-Wl,--gc-sections", "-Wl,--version-script=exports.map",
                   "-Wl,--dependency-file=image.d", "-o", output, "module.o", "-lm", cwd=self.work)

    def test_image_links_and_runs(self):
        result = self.link()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("2 of 3 exports", result.stdout)
        defined = run(NM, "-g", "--defined-only", "image.o", cwd=self.work).stdout.split()
        self.assertEqual(sorted(name for name in defined if name.startswith("charged")),
                         ["charged_image_test__charged_entry", "charged_image_test__charged_throw"])
        symbols = run(NM, "image.o", cwd=self.work).stdout
        self.assertNotIn("charged_unused_helper", symbols)  # collected like the module's --gc-sections
        self.assertNotIn(" T main", symbols)
        self.assertTrue((self.work / "image.d").is_file())
        sections = run(READELF, "-SW", "image.o", cwd=self.work).stdout
        for name in ("charged_image_test_text", "charged_image_test_init", ".eh_frame"):
            self.assertIn(name, sections)
        result = run(CXX, "-O2", "program.cpp", "image.o", "-o", "program", cwd=self.work)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(run(str(self.work / "program")).returncode, 0)

    def test_provider_the_image_defines_is_refused(self):
        (self.work / "own.cpp").write_text("CHARGED_MISSING_PROVIDER(charged_throw)\n")
        result = self.link(providers="own.cpp")
        self.assertEqual(result.returncode, 1)
        self.assertIn("now defines charged_throw", result.stderr)
        self.assertFalse((self.work / "image.o").exists())

    def test_shared_library_input_is_refused(self):
        result = run(sys.executable, "-B", str(TOOL), "--ld", LD, "--objcopy", OBJCOPY, "--nm", NM,
                     "--readelf", READELF, "--image", "test", "--", CXX, "-shared",
                     "-Wl,--version-script=exports.map", "-o", "image.o", "module.o", "libfoo.so",
                     cwd=self.work)
        self.assertEqual(result.returncode, 1)
        self.assertIn("shared library libfoo.so", result.stderr)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
