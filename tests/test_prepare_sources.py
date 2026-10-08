#!/usr/bin/env python3
"""Exercise source preparation against disposable local Git repositories."""
import difflib
import errno
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import tracemalloc
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from format_prepared import find_clang_format
from prepare_sources import PreparationError, content_inventory, prepare


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

    def test_inventory_bounds_memory_and_preserves_bytes_modes(self):
        tree = self.workspace / "inventory"
        tree.mkdir()
        payload = tree / "payload.bin"
        block = bytes(range(256)) * 16
        expected = hashlib.sha256()
        with payload.open("wb") as stream:
            for _ in range(1024):
                stream.write(block)
                expected.update(block)
            stream.write(b"unaligned tail\0\xff")
            expected.update(b"unaligned tail\0\xff")
        payload.chmod(0o755)
        executable = bool(payload.stat().st_mode & 0o111)
        (tree / "empty").write_bytes(b"")
        tracemalloc.start()
        try:
            inventory = content_inventory(tree)
            peak = tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()
        self.assertLess(peak, 1024 * 1024, "Inventory retained a file-sized Python allocation")
        self.assertEqual(inventory, {
            "empty": {"sha256": hashlib.sha256(b"").hexdigest(), "executable": False},
            "payload.bin": {"sha256": expected.hexdigest(), "executable": executable},
        })

    def test_inventory_preserves_symlinks_without_following_them(self):
        tree = self.workspace / "inventory"
        tree.mkdir()
        try:
            (tree / "outside").symlink_to(self.upstream, target_is_directory=True)
            (tree / "broken").symlink_to("missing")
        except OSError as error:
            if os.name == "nt":
                self.skipTest(f"Symlink fixture unavailable: {error}")
            raise
        self.assertEqual(content_inventory(tree), {
            "outside": {"symlink": str(self.upstream)}, "broken": {"symlink": "missing"},
        })

    def test_inventory_propagates_open_read_and_final_stat_failures(self):
        tree = self.workspace / "inventory"
        tree.mkdir()
        path = tree / "value.bin"
        path.write_bytes(b"source bytes")
        failed = OSError(errno.EIO, "injected file failure")
        with mock.patch.object(Path, "open", side_effect=failed):
            with self.assertRaises(OSError) as caught:
                content_inventory(tree)
            self.assertIs(caught.exception, failed)

        class FailedReader:
            calls = 0

            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def read(self, size=-1):
                self.calls += 1
                if self.calls == 1:
                    return b"partial read"
                raise failed

        with mock.patch.object(Path, "open", return_value=FailedReader()):
            with self.assertRaises(OSError) as caught:
                content_inventory(tree)
            self.assertIs(caught.exception, failed)

        original_stat = Path.stat
        def fail_final_stat(queried, *args, **kwargs):
            if queried == path and kwargs.get("follow_symlinks", True):
                raise failed  # The unchanged executable-mode check after hashing.
            return original_stat(queried, *args, **kwargs)

        with mock.patch.object(Path, "is_file", return_value=True), \
                mock.patch.object(Path, "stat", fail_final_stat):
            with self.assertRaises(OSError) as caught:
                content_inventory(tree)
            self.assertIs(caught.exception, failed)

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

    def test_refresh_preserves_only_identical_verified_file_timestamps(self):
        source = self.run_prepare()
        stamp = 1_500_000_000_000_000_000
        for name in ("value.txt", "remove.txt"):
            os.utime(source / name, ns=(stamp, stamp))
        self.patch("one.patch", "original\n", "changed\n")
        self.run_prepare()
        self.assertEqual((source / "remove.txt").stat().st_mtime_ns, stamp)
        self.assertNotEqual((source / "value.txt").stat().st_mtime_ns, stamp)
        self.assertEqual((source / "value.txt").read_text(), "changed\n")
        (source / "remove.txt").write_text("unpreserved local edit\n")
        os.utime(source / "remove.txt", ns=(stamp, stamp))
        self.run_prepare(discard_generated=True)
        self.assertNotEqual((source / "remove.txt").stat().st_mtime_ns, stamp)
        self.run_prepare(check=True)

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

    def decomp_fixture(self):
        for name in ("include/types.h", "libs/sdk.h", "src/game.cpp", "config/build.yml", "orig/disc-note.txt", "README.md"):
            path = self.upstream / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("synthetic fixture\n", newline="\n")
        self.git(self.upstream, "add", ".")
        self.git(self.upstream, "commit", "-qm", "Decomp fixture")
        self.git(self.root, "-c", "protocol.file.allow=always", "submodule", "add",
                 "-q", str(self.upstream), "extern/mscharged-decomp")
        patch_dir = self.root / "patches/mscharged-decomp"
        patch_dir.mkdir(parents=True)
        pin = self.git(self.upstream, "rev-parse", "HEAD").strip()
        (patch_dir / "base").write_text(pin + "\n", newline="\n")
        (patch_dir / "series").write_text("", newline="\n")
        return patch_dir

    def clang_format(self):
        path = os.environ.get("MSCHARGED_TEST_CLANG_FORMAT") or find_clang_format()
        if not path:
            self.skipTest("clang-format 16 or newer is not available")
        return path

    def format_fixture(self):
        files = {
            "include/types.h": "typedef int s32;\n",
            "src/game.cpp": "int add( int a,int b ){return a+b;}\n",
            "src/panic.cpp": "void report(int line);\n#define PANIC() report(__LINE__)\nvoid f( ) { PANIC(); }\n",
        }
        for name, text in files.items():
            path = self.upstream / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, newline="\n")
        self.git(self.upstream, "add", ".")
        self.git(self.upstream, "commit", "-qm", "Formatting fixture")
        self.git(self.root, "-c", "protocol.file.allow=always", "submodule", "add",
                 "-q", str(self.upstream), "extern/mscharged-decomp")
        patch_dir = self.root / "patches/mscharged-decomp"
        patch_dir.mkdir(parents=True)
        (patch_dir / "base").write_text(self.git(self.upstream, "rev-parse", "HEAD").strip() + "\n", newline="\n")
        (patch_dir / "series").write_text("", newline="\n")
        return patch_dir

    def prepare_decomp(self, **kwargs):
        return prepare(self.root, self.build, "mscharged-decomp", **kwargs)

    def test_formatted_compile_tree_keeps_exact_patched_tree(self):
        clang_format = self.clang_format()
        self.format_fixture()
        source = self.prepare_decomp(clang_format=clang_format)
        patched = source.parent / "patched"
        self.assertEqual((patched / "src/game.cpp").read_text(), "int add( int a,int b ){return a+b;}\n")
        self.assertEqual((source / "src/game.cpp").read_text(), "int add(int a, int b)\n{\n    return a + b;\n}\n")
        # Formatting would move a __LINE__ expansion, so the file stays exactly as patched.
        self.assertEqual((source / "src/panic.cpp").read_bytes(), (patched / "src/panic.cpp").read_bytes())
        state = json.loads((source.parent / "manifest.json").read_text())
        self.assertEqual(state["kept_unformatted"], {"src/panic.cpp": "PANIC line moved"})
        self.assertIn("clang-format version", state["inputs"]["formatting"]["tool"])
        self.prepare_decomp(clang_format=clang_format, check=True)
        with self.assertRaisesRegex(PreparationError, "stale or modified"):
            self.prepare_decomp(check=True)  # An unformatted build expects other sources.

    def test_unformatted_game_sources_keep_both_trees(self):
        patch_dir = self.format_fixture()
        source = self.prepare_decomp()  # No clang-format: source/ is an unchanged copy.
        patched = source.parent / "patched"
        self.assertEqual((source / "src/game.cpp").read_bytes(), (patched / "src/game.cpp").read_bytes())
        self.assertNotIn("kept_unformatted", json.loads((source.parent / "manifest.json").read_text()))
        (patched / "src/game.cpp").write_text("int add( int a,int b ){return a*b;}\n", newline="\n")
        exported = self.prepare_decomp(export_patch=self.build / "edit.patch")
        shutil.copyfile(exported, patch_dir / "0001-edit.patch")
        (patch_dir / "series").write_text("0001-edit.patch\n", newline="\n")
        source = self.prepare_decomp(discard_generated=True)
        self.assertEqual((source / "src/game.cpp").read_text(), "int add( int a,int b ){return a*b;}\n")
        self.prepare_decomp(check=True)

    def test_patch_development_uses_exact_tree(self):
        clang_format = self.clang_format()
        patch_dir = self.format_fixture()
        source = self.prepare_decomp(clang_format=clang_format)
        patched = source.parent / "patched"
        (source / "src/game.cpp").write_text("int add(int a, int b) { return 0; }\n", newline="\n")
        with self.assertRaisesRegex(PreparationError, "stale or modified"):
            self.prepare_decomp(clang_format=clang_format, check=True)
        with self.assertRaisesRegex(PreparationError, "Make patch edits in"):
            self.prepare_decomp(export_patch=self.build / "rejected.patch")
        source = self.prepare_decomp(clang_format=clang_format, discard_generated=True)
        (patched / "src/game.cpp").write_text("int add( int a,int b ){return a-b;}\n", newline="\n")
        exported = self.prepare_decomp(export_patch=self.build / "edit.patch")  # Any formatter, or none.
        self.assertIn("-int add( int a,int b ){return a+b;}", exported.read_text())
        shutil.copyfile(exported, patch_dir / "0001-edit.patch")
        (patch_dir / "series").write_text("0001-edit.patch\n", newline="\n")
        source = self.prepare_decomp(clang_format=clang_format, discard_generated=True)
        self.assertEqual((patched / "src/game.cpp").read_text(), "int add( int a,int b ){return a-b;}\n")
        self.assertEqual((source / "src/game.cpp").read_text(), "int add(int a, int b)\n{\n    return a - b;\n}\n")
        self.prepare_decomp(clang_format=clang_format, check=True)

    def test_formatting_reuses_unchanged_results(self):
        clang_format = self.clang_format()
        patch_dir = self.format_fixture()
        source = self.prepare_decomp(clang_format=clang_format)
        stamp = 1_500_000_000_000_000_000
        for name in ("include/types.h", "src/game.cpp"):
            os.utime(source / name, ns=(stamp, stamp))
        text = "int add( int a,int b ){return a+b;}\n"
        changed = text.replace("a+b", "b+a")
        patch = "".join(difflib.unified_diff(text.splitlines(keepends=True), changed.splitlines(keepends=True),
                                             fromfile="a/src/game.cpp", tofile="b/src/game.cpp"))
        (patch_dir / "0001-swap.patch").write_text(patch, newline="\n")
        (patch_dir / "series").write_text("0001-swap.patch\n", newline="\n")
        self.prepare_decomp(clang_format=clang_format)
        self.assertEqual((source / "include/types.h").stat().st_mtime_ns, stamp)
        self.assertNotEqual((source / "src/game.cpp").stat().st_mtime_ns, stamp)
        self.assertIn("return b + a;", (source / "src/game.cpp").read_text())
        cache = self.build / "prepared/.mscharged-decomp.format-cache"
        entries = sorted(path.suffix for path in cache.rglob("*") if path.is_file())
        self.assertEqual(entries, ["", "", ".kept"])  # Only the current tree's results remain.

    def test_formatting_definition_is_an_input(self):
        clang_format = self.clang_format()
        self.format_fixture()
        self.prepare_decomp(clang_format=clang_format)
        definition = self.build / "two-spaces.clang-format"
        definition.write_text("BasedOnStyle: WebKit\nIndentWidth: 2\nBreakBeforeBraces: Allman\n"
                              "AllowShortFunctionsOnASingleLine: None\n", newline="\n")
        source = self.prepare_decomp(clang_format=clang_format, format_config=definition)
        self.assertEqual((source / "src/game.cpp").read_text(), "int add(int a, int b)\n{\n  return a + b;\n}\n")
        with self.assertRaisesRegex(PreparationError, "stale or modified"):
            self.prepare_decomp(clang_format=clang_format, check=True)

    def nested_fixture(self):
        nested = self.workspace / "nested"
        nested.mkdir()
        self.git(nested, "init", "-q", "-b", "main")
        (nested / "child.txt").write_text("pinned child\n", newline="\n")
        self.git(nested, "add", ".")
        self.git(nested, "commit", "-qm", "Nested fixture")
        for name in ("required", "optional"):
            self.git(self.upstream, "-c", "protocol.file.allow=always", "submodule", "add",
                     "-q", str(nested), f"third_party/{name}")
        self.git(self.upstream, "commit", "-qam", "Nested inputs")
        self.git(self.submodule, "fetch", "-q")
        self.git(self.submodule, "checkout", "-q", "FETCH_HEAD")
        self.git(self.root, "add", "extern/example")
        self.git(self.submodule, "-c", "protocol.file.allow=always", "submodule", "update",
                 "--init", "--checkout", "--", "third_party/required")
        (self.patch_dir / "base").write_text(self.git(self.submodule, "rev-parse", "HEAD").strip() + "\n")

    def test_explicit_nested_subset_omits_uninitialized_optional_inputs(self):
        self.nested_fixture()
        with self.assertRaisesRegex(PreparationError, "Initialize the pinned submodule"):
            self.run_prepare()
        selected = ["third_party/required"]
        source = self.run_prepare(nested_submodules=selected)
        self.assertEqual((source / "third_party/required/child.txt").read_text(), "pinned child\n")
        self.assertFalse((source / "third_party/optional/child.txt").exists())
        state = json.loads((source.parent / "manifest.json").read_text())
        self.assertEqual(state["inputs"]["nested_submodules"], selected)
        self.run_prepare(nested_submodules=selected, check=True)
        with self.assertRaisesRegex(PreparationError, "stale or modified"):
            self.run_prepare(nested_submodules=[], check=True)

    def test_selected_nested_input_still_requires_exact_clean_pin(self):
        self.nested_fixture()
        child = self.submodule / "third_party/required"
        (child / "child.txt").write_text("changed\n")
        with self.assertRaisesRegex(PreparationError, "Tracked changes"):
            self.run_prepare(nested_submodules=["third_party/required"])

    def test_nested_subset_rejects_unknown_and_duplicate_gitlinks(self):
        with self.assertRaisesRegex(PreparationError, "not a recorded gitlink"):
            self.run_prepare(nested_submodules=["../outside"])
        with self.assertRaisesRegex(PreparationError, "Duplicate selected"):
            self.run_prepare(nested_submodules=["missing", "missing"])

    def test_decomp_export_contains_only_selected_source_roots(self):
        self.decomp_fixture()
        source = prepare(self.root, self.build, "mscharged-decomp")
        self.assertEqual({path.name for path in source.iterdir()}, {"include", "libs", "src"})
        self.assertTrue((source / "src/game.cpp").is_file())
        self.assertTrue((self.root / "extern/mscharged-decomp/config/build.yml").is_file())
        self.assertEqual(self.git(self.root / "extern/mscharged-decomp", "status", "--porcelain"), "")
        state = json.loads((source.parent / "manifest.json").read_text())
        self.assertEqual(state["inputs"]["export_roots"], ["include", "libs", "src"])
        prepare(self.root, self.build, "mscharged-decomp", check=True)

    def test_decomp_patch_cannot_reintroduce_omitted_directories(self):
        patch_dir = self.decomp_fixture()
        (patch_dir / "outside.patch").write_text(
            "--- /dev/null\n+++ b/README.md\n@@ -0,0 +1 @@\n+Outside the selected build source\n", newline="\n")
        (patch_dir / "series").write_text("outside.patch\n", newline="\n")
        with self.assertRaisesRegex(PreparationError, "outside the selected source directories"):
            prepare(self.root, self.build, "mscharged-decomp")
        self.assertFalse((self.build / "prepared/mscharged-decomp").exists())


if __name__ == "__main__":
    unittest.main()
