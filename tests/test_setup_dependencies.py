#!/usr/bin/env python3
"""Exercise pinned, nonrecursive setup against disposable offline Git sources."""
from __future__ import annotations

from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from setup_dependencies import DAWN_COMMON, DAWN_VULKAN, LAUNCHER, RUNTIME, SetupError, setup


class DependencySetupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="mscharged-dependencies-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.base = Path(cls.temp.name)
        # These fixtures can access local paths only. No network is used, and
        # system/user Git options cannot add recursive updates or credentials.
        cls.environment = {
            "GIT_ALLOW_PROTOCOL": "file", "GIT_TERMINAL_PROMPT": "0",
            "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_AUTHOR_NAME": "Offline fixture", "GIT_AUTHOR_EMAIL": "fixture@example.invalid",
            "GIT_COMMITTER_NAME": "Offline fixture", "GIT_COMMITTER_EMAIL": "fixture@example.invalid",
        }
        cls.env_patch = patch.dict(os.environ, cls.environment)
        cls.env_patch.start()
        cls.addClassCleanup(cls.env_patch.stop)
        cls.leaf = cls.base / "leaf"
        cls.init(cls.leaf)
        (cls.leaf / "payload.txt").write_text("recorded payload\n")
        cls.commit(cls.leaf)
        cls.leaf_pin = cls.git(cls.leaf, "rev-parse", "HEAD").strip()
        cls.dawn = cls.base / "dawn"
        cls.init(cls.dawn)
        for path in (*DAWN_COMMON, *DAWN_VULKAN, "buildtools"):
            cls.git(cls.dawn, "submodule", "add", str(cls.leaf), path)
        cls.commit(cls.dawn)
        cls.freetype = cls.base / "freetype"
        cls.init(cls.freetype)
        cls.git(cls.freetype, "submodule", "add", str(cls.leaf), "subprojects/dlg")
        cls.commit(cls.freetype)
        cls.template = cls.base / "parent"
        cls.init(cls.template)
        for path in (*RUNTIME, "extern/qtbase"):
            source = cls.dawn if path == "extern/dawn" else cls.freetype if path == "extern/freetype" else cls.leaf
            cls.git(cls.template, "submodule", "add", str(source), path)
        cls.commit(cls.template)
        # Remote branch tips now disagree with the recorded parent gitlinks.
        (cls.leaf / "payload.txt").write_text("new remote tip\n")
        cls.commit(cls.leaf)
        cls.leaf_tip = cls.git(cls.leaf, "rev-parse", "HEAD").strip()

    @classmethod
    def git(cls, repo, *args):
        result = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        return result.stdout

    @classmethod
    def init(cls, repo):
        repo.mkdir(parents=True)
        cls.git(repo, "init", "--initial-branch=main")

    @classmethod
    def commit(cls, repo):
        cls.git(repo, "add", "--all")
        cls.git(repo, "commit", "--quiet", "-m", "Offline fixture")

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="checkout-", dir=self.base)
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / "port"
        self.git(self.base, "clone", "--no-recurse-submodules", str(self.template), str(self.root))
        # The helper must override a user's recursive-default preference.
        self.git(self.root, "config", "submodule.recurse", "true")

    def run_setup(self, **kwargs):
        with redirect_stdout(io.StringIO()):
            setup(self.root, **kwargs)

    def initialized(self, path):
        return (self.root / path / ".git").exists()

    def assert_leaf_pin(self, path):
        self.assertEqual(self.git(self.root / path, "rev-parse", "HEAD").strip(), self.leaf_pin)
        self.assertEqual((self.root / path / "payload.txt").read_text(), "recorded payload\n")

    def test_linux_recorded_pins_and_no_blanket_recursion(self):
        self.run_setup(system="Linux")
        self.assertTrue(all(self.initialized(path) for path in RUNTIME))
        for path in (*DAWN_COMMON, *DAWN_VULKAN):
            self.assert_leaf_pin("extern/dawn/" + path)
        self.assert_leaf_pin("extern/nod")
        self.assert_leaf_pin("extern/freetype/subprojects/dlg")
        self.assertFalse(self.initialized("extern/dawn/buildtools"))
        self.assertFalse(self.initialized("extern/qtbase"))

    def test_macos_only_selected_metal_dependencies(self):
        self.run_setup(system="Darwin")
        # Dawn patch 0002 edits this tree even when its backend is not built.
        self.assert_leaf_pin("extern/dawn/third_party/spirv-tools/src")
        for path in DAWN_COMMON:
            self.assert_leaf_pin("extern/dawn/" + path)
        for path in (*DAWN_VULKAN, "buildtools"):
            self.assertFalse(self.initialized("extern/dawn/" + path))
        self.assert_leaf_pin("extern/freetype/subprojects/dlg")

    def test_launcher_does_not_initialize_runtime_or_nested_pins(self):
        self.run_setup(launcher=True, system="Windows")
        self.assertTrue(all(self.initialized(path) for path in LAUNCHER))
        self.assertTrue(all(not self.initialized(path) for path in RUNTIME if path not in LAUNCHER))
        self.assertFalse(self.initialized("extern/qtbase"))

    def test_unstaged_and_staged_dependency_edits_preserved_before_any_update(self):
        self.git(self.root, "-c", "submodule.recurse=false", "submodule", "update", "--init", "--checkout", "--", "extern/nod")
        repo = self.root / "extern/nod"
        (repo / "payload.txt").write_text("user work\n")
        for staged in (False, True):
            if staged:
                self.git(repo, "add", "payload.txt")
            with self.assertRaisesRegex(SetupError, "Tracked changes"):
                self.run_setup(system="Linux")
            self.assertEqual((repo / "payload.txt").read_text(), "user work\n")
            self.assertEqual(self.git(repo, "rev-parse", "HEAD").strip(), self.leaf_pin)
            self.assertFalse(self.initialized("extern/mscharged-decomp"))

    def test_nested_edits_preserved_before_direct_initialization(self):
        self.git(self.root, "-c", "submodule.recurse=false", "submodule", "update", "--init", "--checkout", "--", "extern/dawn")
        dawn = self.root / "extern/dawn"
        self.git(dawn, "-c", "submodule.recurse=false", "submodule", "update", "--init", "--checkout", "--", DAWN_COMMON[0])
        nested = dawn / DAWN_COMMON[0]
        (nested / "payload.txt").write_text("nested user work\n")
        with self.assertRaisesRegex(SetupError, "Tracked changes"):
            self.run_setup(system="Linux")
        self.assertEqual((nested / "payload.txt").read_text(), "nested user work\n")
        self.assertFalse(self.initialized("extern/mscharged-decomp"))

    def test_clean_wrong_revision_returns_to_index_and_staged_pin_is_authoritative(self):
        self.run_setup(system="Linux")
        nod = self.root / "extern/nod"
        nested = self.root / "extern/dawn" / DAWN_COMMON[1]
        self.git(nod, "checkout", "--detach", self.leaf_tip)
        self.git(nested, "checkout", "--detach", self.leaf_tip)
        self.run_setup(system="Linux")
        self.assert_leaf_pin("extern/nod")
        self.assert_leaf_pin("extern/dawn/" + DAWN_COMMON[1])
        self.git(nod, "checkout", "--detach", self.leaf_tip)
        self.git(self.root, "add", "extern/nod")
        self.run_setup(system="Linux")
        self.assertEqual(self.git(nod, "rev-parse", "HEAD").strip(), self.leaf_tip)

    def test_staged_nested_gitlink_edit_is_preserved_as_dependency_source_edit(self):
        self.git(self.root, "-c", "submodule.recurse=false", "submodule", "update", "--init", "--checkout", "--", "extern/dawn")
        dawn = self.root / "extern/dawn"
        self.git(dawn, "-c", "submodule.recurse=false", "submodule", "update", "--init", "--checkout", "--", DAWN_COMMON[0])
        nested = dawn / DAWN_COMMON[0]
        self.git(nested, "checkout", "--detach", self.leaf_tip)
        self.git(dawn, "add", DAWN_COMMON[0])
        with self.assertRaisesRegex(SetupError, "Tracked changes"):
            self.run_setup(system="Linux")
        self.assertEqual(self.git(nested, "rev-parse", "HEAD").strip(), self.leaf_tip)
        self.assertFalse(self.initialized("extern/mscharged-decomp"))

    def test_nonrepository_local_files_preserved(self):
        repo = self.root / "extern/nod"
        repo.mkdir(exist_ok=True)
        (repo / "notes.txt").write_text("local notes\n")
        with self.assertRaisesRegex(SetupError, "contains local files"):
            self.run_setup(launcher=True, system="Linux")
        self.assertEqual((repo / "notes.txt").read_text(), "local notes\n")
        self.assertFalse(self.initialized("extern/mscharged-decomp"))

    def test_dirty_submodule_declarations_fail_without_updates(self):
        manifest = self.root / ".gitmodules"
        manifest.write_text(manifest.read_text() + "# local declaration edit\n")
        with self.assertRaisesRegex(SetupError, "Tracked changes"):
            self.run_setup(system="Linux")
        self.assertIn("# local declaration edit", manifest.read_text())
        self.assertFalse(self.initialized("extern/mscharged-decomp"))

    def test_dry_run_and_unsupported_runtime_do_not_initialize(self):
        self.run_setup(system="Darwin", dry_run=True)
        self.assertTrue(all(not self.initialized(path) for path in RUNTIME))
        with self.assertRaisesRegex(SetupError, "Linux/Vulkan or macOS/Metal"):
            self.run_setup(system="Windows")
        self.assertTrue(all(not self.initialized(path) for path in RUNTIME))


if __name__ == "__main__":
    unittest.main()
