#!/usr/bin/env python3
"""Exercise configuration and disc opening without proprietary game data."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


EXECUTABLE = Path(sys.argv.pop(1)).resolve()


def write_disc(path, game_id=b"R4QE01", partition=True):
    """A tiny, unencrypted Wii container with one original text file."""
    data = bytearray(0x60000)
    data[:6] = game_id
    data[7] = 1
    data[0x18:0x1C] = bytes.fromhex("5d1c9ea3")
    title = b"Port test fixture"
    data[0x20:0x20 + len(title)] = title
    data[0x60:0x62] = b"\x01\x01"  # No hashes or encryption.
    if partition:
        struct.pack_into(">II", data, 0x40000, 1, 0x40020 >> 2)
        struct.pack_into(">II", data, 0x40020, 0x50000 >> 2, 0)
        issuer = b"Root-CA00000001-XS00000003"
        data[0x50140:0x50140 + len(issuer)] = issuer
        struct.pack_into(">II", data, 0x502B8, 0x8000 >> 2, 0x8000 >> 2)
        base = 0x58000
        data[base:base + 0x400] = data[:0x400]
        struct.pack_into(">III", data, base + 0x420, 0x2800 >> 2, 0x3000 >> 2, 36 >> 2)
        struct.pack_into(">I", data, base + 0x2800, 0x100)  # Synthetic DOL text offset.
        struct.pack_into(">I", data, base + 0x2800 + 0x90, 4)  # Text size.
        struct.pack_into(">III", data, base + 0x3000, 0x01000000, 0, 2)
        payload = b"Synthetic fixture data.\n"
        struct.pack_into(">III", data, base + 0x300C, 0, 0x3200 >> 2, len(payload))
        data[base + 0x3018:base + 0x3021] = b"test.txt\0"
        data[base + 0x3200:base + 0x3200 + len(payload)] = payload
    path.write_bytes(data)


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
        self.assertRegex(self.run_port("--version"), r"mscharged-port \d+\.\d+\.\d+-dev\+g[0-9a-f]+")
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
