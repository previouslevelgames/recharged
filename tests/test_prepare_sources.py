#!/usr/bin/env python3
"""Exercise source preparation against disposable local Git repositories."""
import difflib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from prepare_sources import PreparationError, prepare


class SourcePreparationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mscharged-prepare-test-")
        self.addCleanup(self.temp.cleanup)
        self.workspace = Path(self.temp.name)
        self.root = self.workspace / "port"
        self.upstream = self.workspace / "upstream"
        self.hooks = self.workspace / "empty-hooks"
        self.hooks.mkdir()
        for directory in (self.root, self.upstream):
            directory.mkdir()
            self.git(directory, "init", "-q", "-b", "main")
        (self.upstream / "value.txt").write_text("original\n", newline="\n")
        (self.upstream / "remove.txt").write_text("preserve until deliberately removed\n", newline="\n")
        (self.upstream / ".gitignore").write_text("scratch.txt\n", newline="\n")
        self.git(self.upstream, "add", ".")
        self.git(self.upstream, "commit", "-qm", "Fixture")
        self.pin = self.git(self.upstream, "rev-parse", "HEAD").strip()
        self.git(self.root, "-c", "protocol.file.allow=always", "submodule", "add",
                 "-q", str(self.upstream), "extern/example")
        self.submodule = self.root / "extern/example"
        self.build = self.root / "build"
        self.patch_dir = self.root / "patches/example"
        self.patch_dir.mkdir(parents=True)
        (self.patch_dir / "base").write_text(self.pin + "\n", newline="\n")
        (self.patch_dir / "series").write_text("", newline="\n")

    def git(self, directory, *args):
        return subprocess.check_output(
            ["git", "-C", str(directory), "-c", "user.name=Source preparation tests",
             "-c", "user.email=tests@example.invalid", "-c", "commit.gpgsign=false",
             "-c", f"core.hooksPath={self.hooks}", "-c", "core.autocrlf=false",
             "-c", "submodule.recurse=false",
             *args], stderr=subprocess.PIPE, text=True,
        )

    def patch(self, name, old, new):
        text = "".join(difflib.unified_diff(old.splitlines(keepends=True), new.splitlines(keepends=True),
                                          fromfile="a/value.txt", tofile="b/value.txt"))
        (self.patch_dir / name).write_text(text, newline="\n")
        with (self.patch_dir / "series").open("a") as series:
            series.write(name + "\n")

    def run_prepare(self, **kwargs):
        return prepare(self.root, self.build, "example", **kwargs)

    def test_ordered_patches_and_cache(self):
        self.patch("one.patch", "original\n", "first\n")
        self.patch("two.patch", "first\n", "second\n")
        source = self.run_prepare()
        self.assertEqual((source / "value.txt").read_text(), "second\n")
        modified = (source / "value.txt").stat().st_mtime_ns
        self.run_prepare()
        self.assertEqual((source / "value.txt").stat().st_mtime_ns, modified)
        self.assertEqual((self.submodule / "value.txt").read_text(), "original\n")
        self.assertEqual(self.git(self.submodule, "status", "--porcelain"), "")
        self.run_prepare(check=True)

    def test_untracked_and_ignored_files_are_not_exported(self):
        (self.submodule / "scratch.txt").write_text("ignored data\n", newline="\n")
        (self.submodule / "local.txt").write_text("untracked data\n", newline="\n")
        source = self.run_prepare()
        self.assertFalse((source / "scratch.txt").exists())
        self.assertFalse((source / "local.txt").exists())

    def test_tracked_edit_rejected(self):
        (self.submodule / "value.txt").write_text("local edit\n", newline="\n")
        with self.assertRaisesRegex(PreparationError, "Tracked changes"):
            self.run_prepare()

    def test_local_export_attributes_cannot_hide_committed_source(self):
        attributes = self.git(self.submodule, "rev-parse", "--git-path", "info/attributes").strip()
        path = (self.submodule / attributes).resolve()
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("value.txt export-ignore\n", newline="\n")
        source = self.run_prepare()
        self.assertEqual((source / "value.txt").read_text(), "original\n")

    def test_unexpected_revision_rejected(self):
        (self.submodule / "value.txt").write_text("new commit\n", newline="\n")
        self.git(self.submodule, "add", "value.txt")
        self.git(self.submodule, "commit", "-qm", "Unexpected revision")
        with self.assertRaisesRegex(PreparationError, "Unexpected revision"):
            self.run_prepare()

    def test_wrong_patch_base_rejected(self):
        (self.patch_dir / "base").write_text("0" * 40 + "\n", newline="\n")
        with self.assertRaisesRegex(PreparationError, "Patch base"):
            self.run_prepare()

    def test_failure_does_not_publish_partial_or_accept_stale_tree(self):
        source = self.run_prepare()
        self.patch("one.patch", "original\n", "first\n")
        self.patch("broken.patch", "not present\n", "second\n")
        with self.assertRaisesRegex(PreparationError, "Patch failed"):
            self.run_prepare()
        self.assertEqual((source / "value.txt").read_text(), "original\n")
        with self.assertRaisesRegex(PreparationError, "stale or modified"):
            self.run_prepare(check=True)

    def test_reordered_patch_series_rejected(self):
        self.patch("one.patch", "original\n", "first\n")
        self.patch("two.patch", "first\n", "second\n")
        (self.patch_dir / "series").write_text("two.patch\none.patch\n", newline="\n")
        with self.assertRaisesRegex(PreparationError, "Patch failed"):
            self.run_prepare()
        self.assertFalse((self.build / "prepared/example").exists())

    def test_generated_edit_requires_preservation(self):
        source = self.run_prepare()
        (source / "value.txt").write_text("developer work\n", newline="\n")
        with self.assertRaisesRegex(PreparationError, "Generated sources were edited"):
            self.run_prepare()
        with self.assertRaisesRegex(PreparationError, "stale or modified"):
            self.run_prepare(check=True)

    def test_export_and_refresh_add_modify_delete(self):
        source = self.run_prepare()
        (source / "value.txt").write_text("developer work\n", newline="\n")
        (source / "new.txt").write_text("new source\n", newline="\n")
        (source / "remove.txt").unlink()
        exported = self.run_prepare(export_patch=self.build / "export.patch")
        shutil.copyfile(exported, self.patch_dir / "change.patch")
        (self.patch_dir / "series").write_text("change.patch\n", newline="\n")
        source = self.run_prepare(discard_generated=True)
        self.assertEqual((source / "value.txt").read_text(), "developer work\n")
        self.assertEqual((source / "new.txt").read_text(), "new source\n")
        self.assertFalse((source / "remove.txt").exists())
        self.run_prepare(check=True)

    def test_patch_path_escape_rejected(self):
        (self.patch_dir / "series").write_text("../../outside.patch\n", newline="\n")
        with self.assertRaisesRegex(PreparationError, "escapes"):
            self.run_prepare()

    def test_output_cannot_replace_source(self):
        with self.assertRaises(PreparationError):
            prepare(self.root, self.root / "extern/example", "example")

    def test_nested_submodule_export(self):
        nested = self.workspace / "nested"
        nested.mkdir()
        self.git(nested, "init", "-q", "-b", "main")
        (nested / "nested.txt").write_text("nested source\n", newline="\n")
        self.git(nested, "add", ".")
        self.git(nested, "commit", "-qm", "Nested fixture")
        self.git(self.submodule, "-c", "protocol.file.allow=always", "submodule", "add",
                 "-q", str(nested), "nested")
        self.git(self.submodule, "commit", "-qam", "Record nested pin")
        self.git(self.root, "add", "extern/example")
        pin = self.git(self.submodule, "rev-parse", "HEAD").strip()
        (self.patch_dir / "base").write_text(pin + "\n", newline="\n")
        source = self.run_prepare()
        self.assertEqual((source / "nested/nested.txt").read_text(), "nested source\n")
        state = json.loads((source.parent / "manifest.json").read_text())
        self.assertEqual(len(state["inputs"]["sources"]), 2)


if __name__ == "__main__":
    unittest.main()
