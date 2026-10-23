#!/usr/bin/env python3
"""tools/rollback_sections.py on Mach-O objects, built here byte by byte.

The launcher renames the writable data and zeroed data sections of the
original game's objects; these objects hold the sections a compiler emits
(__TEXT,__text, __DATA,__data, __DATA,__bss, __DATA,__common, __DATA,__const)
and a symbol table, which is all the launcher reads.
"""
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import rollback_sections  # noqa: E402


def macho(sections, symbols=()):
    """A thin 64-bit Mach-O object: (segment, section) pairs, and (name, type, value) symbols."""
    strings = b"\0" + b"".join(name + b"\0" for name, _, _ in symbols)
    segment_size = 72 + 80 * len(sections)
    commands = segment_size + 24
    symbols_at = 32 + commands
    strings_at = symbols_at + 16 * len(symbols)
    image = struct.pack("<IiiIIII I", 0xFEEDFACF, 0x0100000C, 0, 1, 2, commands, 0, 0)
    image += struct.pack("<II16sQQQQIIII", 0x19, segment_size, b"", 0, 0, 0, 0, 7, 7, len(sections), 0)
    for segment, section in sections:
        image += struct.pack("<16s16sQQIIIIIIII", section, segment, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    image += struct.pack("<IIIIII", 0x2, 24, symbols_at, len(symbols), strings_at, len(strings))
    strx = 1
    for name, kind, value in symbols:
        image += struct.pack("<IBBHQ", strx, kind, 0, 0, value)
        strx += len(name) + 1
    return image + strings


def section_names(image, base=0):
    sections, = struct.unpack_from("<I", image, base + 32 + 64)
    names = []
    for index in range(sections):
        at = base + 32 + 72 + index * 80
        names.append((image[at + 16:at + 32].rstrip(b"\0"), image[at:at + 16].rstrip(b"\0")))
    return names


SECTIONS = [(b"__TEXT", b"__text"), (b"__DATA", b"__data"), (b"__DATA", b"__bss"),
            (b"__DATA", b"__common"), (b"__DATA", b"__const")]
RENAMED = [(b"__TEXT", b"__text"), (b"__DATA", b"__rbkd"), (b"__DATA", b"__rbkb"),
           (b"__DATA", b"__rbkc"), (b"__DATA", b"__const")]


class MachOTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def write(self, image):
        path = os.path.join(self.directory.name, "object.o")
        with open(path, "wb") as file:
            file.write(image)
        return path

    def test_renames_the_writable_data(self):
        """__data, __bss and __common get names of their own; code and constants keep theirs."""
        path = self.write(macho(SECTIONS, [(b"_gScore", 0x0F, 0), (b"_Seed", 0x01, 0)]))
        rollback_sections.rename_sections(path)
        self.assertEqual(section_names(Path(path).read_bytes()), RENAMED)

    def test_refuses_common_symbols(self):
        """A common symbol (external, undefined, with a size) has no section to rename."""
        path = self.write(macho(SECTIONS, [(b"_gTentative", 0x01, 4)]))
        before = Path(path).read_bytes()
        with self.assertRaises(SystemExit) as raised:
            rollback_sections.rename_sections(path)
        self.assertIn("_gTentative", str(raised.exception))
        self.assertEqual(Path(path).read_bytes(), before)

    def test_universal_object(self):
        """Every architecture of a universal object is renamed."""
        thin = macho(SECTIONS)
        first, second = 4096, 8192
        image = struct.pack(">II", 0xCAFEBABE, 2)
        image += struct.pack(">IIIII", 0x0100000C, 0, first, len(thin), 12)
        image += struct.pack(">IIIII", 0x01000007, 3, second, len(thin), 12)
        image = image.ljust(first, b"\0") + thin
        image = image.ljust(second, b"\0") + thin
        path = self.write(image)
        rollback_sections.rename_sections(path)
        renamed = Path(path).read_bytes()
        self.assertEqual(section_names(renamed, first), RENAMED)
        self.assertEqual(section_names(renamed, second), RENAMED)


if __name__ == "__main__":
    unittest.main()
