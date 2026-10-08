"""Offline ELF/Mach-O symbol fixtures; no game/device execution."""

import importlib.util
import pathlib
import unittest

SOURCE = pathlib.Path(__file__).resolve().parents[1] / "tools/check_original_module_vtables.py"
SPEC = importlib.util.spec_from_file_location("module_vtables", SOURCE)
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class VtableImports(unittest.TestCase):
    def test_owner_actual_macho_imports(self):
        imports = """__ZTV20WorldObject_80129EE0
__ZTVN10__cxxabiv117__class_type_infoE
__ZTVN10__cxxabiv120__si_class_type_infoE
__ZTVSt12length_error
__ZTVSt12out_of_range
__ZTVSt14overflow_error
__ZTVSt16invalid_argument
"""
        self.assertEqual(CHECKER.unresolved_tables(imports, "macho"),
                         (["__ZTV20WorldObject_80129EE0"], 6))

    def test_current_elf_runtime_versions(self):
        imports = """_ZTVN10__cxxabiv117__class_type_infoE@CXXABI_1.3
_ZTVN10__cxxabiv120__si_class_type_infoE@CXXABI_1.3
_ZTVSt9bad_alloc@GLIBCXX_3.4
"""
        self.assertEqual(CHECKER.unresolved_tables(imports, "elf"), ([], 3))

    def test_vtt_both_formats(self):
        for kind, prefix in (("elf", ""), ("macho", "_")):
            with self.subTest(kind=kind):
                game = prefix + "_ZTT20WorldObject_80129EE0"
                standard = prefix + "_ZTTNSt3__113basic_ostreamIcNS_11char_traitsIcEEEE"
                self.assertEqual(CHECKER.unresolved_tables(game + "\n" + standard, kind),
                                 ([game], 1))

    def test_namespace_lookalikes_are_not_allowed(self):
        imports = """_ZTV12StadiumLight
_ZTV22__cxxabiv1LookalikeClass
_ZTVN12__cxxabiv1xx5ClassE
_ZTVN5stdxx5ClassE
"""
        self.assertEqual(CHECKER.unresolved_tables(imports, "elf"),
                         (sorted(imports.splitlines()), 0))

    def test_exact_uncompressed_std_namespace(self):
        self.assertEqual(CHECKER.unresolved_tables("_ZTVN3std5ClassE", "elf"), ([], 1))

    def test_host_methods_weak_functions_and_rtti_are_untouched(self):
        imports = """OSPanic
GXCopyDisp
_ZN5cTeam17UpdateControllersEv
_ZTI20WorldObject_80129EE0
__gmon_start__
"""
        self.assertEqual(CHECKER.unresolved_tables(imports, "elf"), ([], 0))

    def test_darwin_removes_exactly_one_prefix(self):
        self.assertEqual(CHECKER.unresolved_tables("___ZTV20WorldObject_80129EE0", "macho"),
                         ([], 0))
        self.assertEqual(CHECKER.unresolved_tables("__ZTV20WorldObject_80129EE0", "macho"),
                         (["__ZTV20WorldObject_80129EE0"], 0))

    def test_empty_and_duplicate_output(self):
        self.assertEqual(CHECKER.unresolved_tables("\n", "elf"), ([], 0))
        name = "_ZTV20WorldObject_80129EE0"
        self.assertEqual(CHECKER.unresolved_tables(name + "\n" + name, "elf"), ([name], 0))

    def test_malformed_output_fails(self):
        for line in ("nm: file does not exist", " U __ZTV20WorldObject_80129EE0",
                     "_ZTV20WorldObject_80129EE0 U", "file.so (for architecture arm64):"):
            with self.subTest(line=line), self.assertRaises(ValueError):
                CHECKER.unresolved_tables(line, "macho")


if __name__ == "__main__":
    unittest.main()
