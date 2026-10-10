#!/usr/bin/env python3
"""Exercise configuration and disc opening without proprietary game data."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


EXECUTABLE = Path(sys.argv.pop(1)).resolve()


from disc_fixture import write_disc

class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mscharged-bootstrap-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def run_port(self, *args, expected=0):
        result = subprocess.run([str(EXECUTABLE), *map(str, args)], cwd=self.root,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def test_version_and_self_test_need_no_game_data(self):
        self.assertRegex(self.run_port("--version"), r"mscharged-port \d+\.\d+\.\d+\+g[0-9a-f]+")
        self.assertIn("checks passed", self.run_port("--self-test"))

    def test_missing_configuration_reports_setup(self):
        self.assertIn("Copy mscharged.ini.example", self.run_port(expected=1))

    def test_iso_opens_data_partition(self):
        disc = self.root / "test.iso"
        write_disc(disc)
        output = self.run_port("--disc", disc)
        self.assertIn("R4QE01 revision 1 (ISO)", output)
        self.assertIn("1 files", output)

    def test_default_configuration(self):
        write_disc(self.root / "test.iso")
        (self.root / "mscharged.ini").write_text("[game]\ndisc = test.iso\n")
        self.assertIn("1 files", self.run_port())

    def test_config_relative_path_spaces_and_bom(self):
        config_dir = self.root / "settings"
        config_dir.mkdir()
        write_disc(config_dir / "disc with spaces.iso")
        config = config_dir / "local.ini"
        config.write_bytes(b'\xef\xbb\xbf; local setup\r\n[game]\r\ndisc = "disc with spaces.iso"\r\n')
        self.assertIn("1 files", self.run_port("--config", config))

    def test_empty_and_duplicate_configuration(self):
        config = self.root / "mscharged.ini"
        config.write_text("[game]\ndisc =\n")
        self.assertIn("Set [game] disc", self.run_port(expected=1))
        config.write_text("[game]\ndisc = one.iso\ndisc = two.iso\n")
        self.assertIn("Invalid configuration", self.run_port(expected=1))

    def test_wrong_game_is_rejected(self):
        disc = self.root / "wrong.iso"
        write_disc(disc, game_id=b"TEST01")
        self.assertIn("Expected a Mario Strikers Charged Wii image", self.run_port("--disc", disc, expected=1))

    def test_missing_partition_is_rejected(self):
        disc = self.root / "empty.iso"
        write_disc(disc, partition=False)
        self.assertIn("Cannot open game data partition", self.run_port("--disc", disc, expected=1))

    def test_missing_or_malformed_disc_is_rejected(self):
        disc = self.root / "bad.rvz"
        self.assertIn("Cannot open disc", self.run_port("--disc", disc, expected=1))
        disc.write_bytes(b"RVZ\x01not a disc image")
        self.assertIn("Cannot open disc", self.run_port("--disc", disc, expected=1))

    def test_invalid_arguments(self):
        self.assertIn("Usage:", self.run_port("--disc", expected=2))
        self.assertIn("Usage:", self.run_port("--unknown", expected=2))


if __name__ == "__main__":
    unittest.main()
