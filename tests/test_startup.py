#!/usr/bin/env python3
"""Check original startup boundaries using synthetic Wii data only."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from disc_fixture import write_disc

EXECUTABLE = Path(sys.argv.pop(1)).resolve()


class StartupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mscharged-startup-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def run_startup(self, config, expected):
        result = subprocess.run([str(EXECUTABLE), "--experimental-startup", "--config", str(config)],
                                cwd=self.root, capture_output=True, text=True, timeout=20)
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, expected, output)
        return output

    def config(self, language="auto", game_id=b"R4QE01"):
        write_disc(self.root / "disc with spaces.iso", game_id=game_id, files={
            "test.txt": b"Synthetic fixture data.\n",
            "ini/common.ini": b"; Synthetic only\n[test]\nvalue = 7\n",
            "ini/datetime.ini": b"; Synthetic only\n[build]\ndate = fixture\n",
        })
        config = self.root / "personal.ini"
        config.write_text(f"; preserve this personal file\n[game]\ndisc = disc with spaces.iso\nlanguage = {language}\n")
        return config

    def test_original_core_completes_and_reaches_remaining_initialization(self):
        config = self.config()
        before = config.read_bytes()
        output = self.run_startup(config, expected=3)
        self.assertIn("Wii image mounted through Aurora DVD/nod", output)
        self.assertIn("InitializeCore() -> nlInit() -> nlInitMemory()", output)
        self.assertIn("Original nlInitMemory completed; MEM1 game arena:", output)
        self.assertIn("MEM2 game arena:", output)
        self.assertIn("reserved SDK heap initialized", output)
        self.assertIn("Original InitializeCore() and nlInit() completed", output)
        self.assertIn("Original event registry, callback transfer, delivery, self-disconnect and state cleanup verified; both arenas recovered", output)
        self.assertIn("Original DispatchEventsTask delivery, reset and queued payload cleanup verified; both arenas recovered", output)
        self.assertIn("Original task manager scheduled DispatchEventsTask across state masks and batches; inactive movie path and arena recovery verified", output)
        self.assertIn("Native SAnim decoders verified: 16/12/8-bit rotations, unsigned scale and byte weights", output)
        self.assertIn("Original nlInitFileSystem completed; NL sync/async reads verified: /ini/common.ini", output)
        self.assertIn("34 of 34 bytes; FNV-1a 0x30853692", output)
        self.assertIn("callback on servicing thread", output)
        self.assertIn("Native NL whole-file async loads verified (bytes only): /ini/common.ini", output)
        self.assertIn("/ini/datetime.ini", output)
        self.assertIn("Original boot configuration parsed through sync/async NL loading: 1 matching typed entries; both arenas recovered", output)
        self.assertIn("Original tweak registry parsed datetime configuration: 1 values; borrowed values preserved and both arenas recovered", output)
        self.assertIn("STOPPED at unimplemented service: Initialize (remaining stages)", output)
        self.assertIn("No menu or match was reached", output)
        self.assertEqual(config.read_bytes(), before)

    def test_original_usa_language_selection(self):
        for language, identifier in [("english", 0), ("french", 7), ("spanish", 8)]:
            with self.subTest(language=language):
                output = self.run_startup(self.config(language), expected=3)
                self.assertIn(f"Original text language ID: {identifier}", output)

    def test_missing_boot_ini_is_an_error(self):
        config = self.config()
        write_disc(self.root / "disc with spaces.iso")
        output = self.run_startup(config, expected=1)
        self.assertIn("Missing boot INI: /ini/common.ini", output)
        self.assertNotIn("whole-file async loads verified", output)

    def test_empty_boot_ini_is_an_error(self):
        config = self.config()
        write_disc(self.root / "disc with spaces.iso", files={"test.txt": b"Synthetic fixture data.\n",
                                                            "ini/common.ini": b""})
        output = self.run_startup(config, expected=1)
        self.assertIn("Boot INI is empty or exceeds the diagnostic limit", output)
        self.assertNotIn("whole-file async loads verified", output)

    def test_other_region_is_not_runtime_supported(self):
        output = self.run_startup(self.config(game_id=b"R4QP01"), expected=1)
        self.assertIn("R4QE01 revision 1 only", output)
        self.assertNotIn("Entering original", output)

    def test_invalid_original_configuration_fails_explicitly(self):
        for common, datetime, message in [
            (b"name=" + b"x" * 255, b"date=fixture\n", "Configuration line exceeds 254 bytes"),
            (b"key=a\0b\n", b"date=fixture\n", "Configuration contains an embedded NUL"),
            (b"key=7\n", b"", "Datetime tweak configuration contains no entries"),
        ]:
            with self.subTest(message=message):
                config = self.config()
                write_disc(self.root / "disc with spaces.iso", files={
                    "ini/common.ini": common, "ini/datetime.ini": datetime,
                })
                output = self.run_startup(config, expected=1)
                # The existing byte check rejects an empty boot INI before parsing.
                if not datetime:
                    self.assertIn("Boot INI is empty or exceeds the diagnostic limit", output)
                else:
                    self.assertIn(message, output)

    def test_other_revision_is_not_runtime_supported(self):
        config = self.config()
        disc = self.root / "disc with spaces.iso"
        data = bytearray(disc.read_bytes())
        data[7] = 0
        disc.write_bytes(data)
        output = self.run_startup(config, expected=1)
        self.assertIn("R4QE01 revision 1 only", output)

    def test_unsupported_regional_language(self):
        output = self.run_startup(self.config("japanese"), expected=1)
        self.assertIn("not supported by this USA disc", output)
        self.assertNotIn("Initializing Aurora", output)

    def test_missing_disc_and_configuration_are_errors(self):
        self.assertIn("FAILED:", self.run_startup(self.root / "missing.ini", expected=1))
        config = self.config()
        (self.root / "disc with spaces.iso").unlink()
        self.assertIn("Cannot open disc", self.run_startup(config, expected=1))


if __name__ == "__main__":
    unittest.main()
