#!/usr/bin/env python3
"""Check generated-source protection and publication using a disposable generator."""
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from prepare_sqlite import generate
from prepare_sources import PreparationError


class SQLitePreparationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mscharged-sqlite-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / "build"
        self.source = self.root / "prepared/source"
        self.source.mkdir(parents=True)
        (self.source.parent / "manifest.json").write_text('{"key": "source-v1"}')
        (self.source / "input.c").write_text("int generated_value = 1;\n")
        (self.source / "input.h").write_text("extern int generated_value;\n")
        (self.source / "Makefile.linux-generic").write_text(
            'sqlite3.c: $(TOP)/input.c\n\tcp "$<" "$@"\n'
            'sqlite3.h: $(TOP)/input.h\n\tcp "$<" "$@"\n')
        self.tools = [shutil.which(tool) for tool in ("cc", "make", "tclsh")]
        if not all(self.tools):
            self.skipTest("SQLite preparation requires cc, GNU Make, and Tcl")
        # Source pin/export checks have their own suite. Keep this fixture
        # focused on the generated-source lifecycle without fetching upstream.
        provider = patch("prepare_sqlite.prepare", return_value=self.source)
        provider.start()
        self.addCleanup(provider.stop)

    def run_generate(self, **kwargs):
        return generate(self.root, self.build, *self.tools, **kwargs)

    def test_cache_and_source_update(self):
        target = self.run_generate()
        stamp = (target / "sqlite3.c").stat().st_mtime_ns
        self.assertEqual(self.run_generate(check=True), target)
        self.run_generate()
        self.assertEqual((target / "sqlite3.c").stat().st_mtime_ns, stamp)
        (self.source.parent / "manifest.json").write_text('{"key": "source-v2"}')
        (self.source / "input.c").write_text("int generated_value = 2;\n")
        with self.assertRaisesRegex(PreparationError, "stale or modified"):
            self.run_generate(check=True)
        self.run_generate()
        self.assertEqual((target / "sqlite3.c").read_text(), "int generated_value = 2;\n")

    def test_edited_outputs_require_explicit_discard(self):
        target = self.run_generate()
        (target / "sqlite3.c").write_text("local work\n")
        (target / "unexpected.txt").write_text("extra generated input\n")
        with self.assertRaisesRegex(PreparationError, "stale or modified"):
            self.run_generate(check=True)
        with self.assertRaisesRegex(PreparationError, "were edited"):
            self.run_generate()
        self.assertEqual((target / "sqlite3.c").read_text(), "local work\n")
        self.run_generate(discard_generated=True)
        self.assertFalse((target / "unexpected.txt").exists())
        self.assertEqual((target / "sqlite3.c").read_text(), (self.source / "input.c").read_text())
        self.run_generate(check=True)

    def test_generator_failure_preserves_previous_publication(self):
        target = self.run_generate()
        state = (target / "manifest.json").read_bytes()
        (self.source.parent / "manifest.json").write_text('{"key": "source-v2"}')
        (self.source / "Makefile.linux-generic").write_text('sqlite3.c:\n\tfalse\n')
        with self.assertRaisesRegex(PreparationError, "generation failed"):
            self.run_generate()
        self.assertEqual((target / "manifest.json").read_bytes(), state)
        self.assertEqual((target / "sqlite3.c").read_text(), "int generated_value = 1;\n")
        with self.assertRaisesRegex(PreparationError, "stale or modified"):
            self.run_generate(check=True)
        self.assertEqual(list((self.build / "generated").glob(".sqlite-generate-*")), [])

    def test_tool_version_change_invalidates_cache(self):
        target = self.run_generate()
        from prepare_sqlite import tool_info
        def changed_info(*args, **kwargs):
            result = tool_info(*args, **kwargs)
            result["version"] += "\nfixture tool upgrade"
            return result
        with patch("prepare_sqlite.tool_info", side_effect=changed_info):
            with self.assertRaisesRegex(PreparationError, "stale or modified"):
                self.run_generate(check=True)
            self.run_generate()
            self.run_generate(check=True)
        self.assertIn("fixture tool upgrade", json.loads((target / "manifest.json").read_text())["inputs"]["cc"]["version"])


if __name__ == "__main__":
    unittest.main()
