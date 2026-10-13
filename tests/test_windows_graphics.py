"""Pinned Windows DLL acquisition checks using only local, synthetic archives."""

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import setup_windows_graphics as graphics


class WindowsGraphicsTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.output = self.root / "output"
        self.cache = self.root / "cache"
        archive = self.root / "release.zip"
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("bin\\x64\\compiler.dll", b"compiler")
            output.writestr("LICENSE.txt", b"license")
            output.writestr("../unrequested.txt", b"must not be extracted")
        self.pin = {"architecture": "x86_64", "archives": [{
            "name": "compiler", "url": archive.as_uri(), "sha256": graphics.digest(archive),
            "files": [{"member": member, "path": path, "sha256": hashlib.sha256(data).hexdigest()}
                      for member, path, data in (("bin\\x64\\compiler.dll", "compiler.dll", b"compiler"),
                                                ("LICENSE.txt", "LICENSES/compiler.txt", b"license"))],
        }], "notices": []}
        self.pin_path = self.root / "pin.json"
        self.pin_path.write_text(json.dumps(self.pin))
        pin_patch = patch.object(graphics, "PIN", self.pin_path)
        pin_patch.start()
        self.addCleanup(pin_patch.stop)

    def test_setup_extracts_only_pinned_files_and_reuses_verified_output_offline(self):
        graphics.setup(self.output, self.cache)
        self.assertEqual((self.output / "compiler.dll").read_bytes(), b"compiler")
        self.assertFalse((self.root / "unrequested.txt").exists())
        with patch.object(graphics.urllib.request, "urlopen", side_effect=AssertionError("network")):
            self.assertEqual(graphics.verify(self.output), self.pin)
            graphics.setup(self.output, self.cache)

    def test_check_rejects_modified_dll_notice_and_manifest(self):
        graphics.setup(self.output, self.cache)
        for relative in ("compiler.dll", "LICENSES/compiler.txt", "windows-graphics.json"):
            target = self.output / relative
            original = target.read_bytes()
            target.write_bytes(b"{}")
            with self.assertRaises(ValueError):
                graphics.verify(self.output)
            target.write_bytes(original)

    def test_rejected_download_preserves_existing_file(self):
        destination = self.root / "existing"
        destination.write_bytes(b"keep")
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            graphics.download(self.pin["archives"][0]["url"], "0" * 64, destination)
        self.assertEqual(destination.read_bytes(), b"keep")

    def test_wrong_archived_dll_does_not_replace_existing_output(self):
        graphics.setup(self.output, self.cache)
        self.pin["archives"][0]["files"][0]["sha256"] = "0" * 64
        self.pin_path.write_text(json.dumps(self.pin))
        with self.assertRaisesRegex(ValueError, "Missing or modified"):
            graphics.setup(self.output, self.cache)
        self.assertEqual((self.output / "compiler.dll").read_bytes(), b"compiler")


if __name__ == "__main__":
    unittest.main()
