#!/usr/bin/env python3
"""Texture packs for tests/texture_packs.cpp, written with Python's zipfile.

Usage: texture_packs_fixture.py TEST_BINARY OUTPUT_DIR

Creates a packs folder with a folder pack, zip packs (deflated, stored, zip64,
backslash names), zips and folders that are no packs, a damaged zip and a
zip whose data is damaged, and a folder that is itself a pack; then runs the
C++ checks against them.
"""

import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

A = "tex1_64x64_m_0123456789abcdef_14"
B = "tex1_128x128_fedcba9876543210_5"
C = "tex1_32x32_1111222233334444_5555666677778888_9"


def data(name):
    # Distinct, compressible contents (the images are only read, not decoded).
    return (name.encode() + b"\n") * 300


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data(text))


def main():
    test, out = sys.argv[1], Path(sys.argv[2])
    shutil.rmtree(out, ignore_errors=True)
    packs = out / "packs"

    folder = packs / "Folder Pack"
    write(folder / "Characters" / f"{A}.png", "folder A")
    write(folder / "Characters" / f"{A}_mip1.png", "folder A mip1")
    write(folder / "Characters" / f"{A}_mip2.png", "folder A mip2")
    write(folder / f"{B}.dds", "folder B dds")
    write(folder / f"{B}.png", "folder B png")
    write(folder / "R4QE01" / f"{C}.png", "folder C (R4QE01)")
    write(folder / "R4QP01" / "tex1_8x8_aaaaaaaaaaaaaaaa_14.png", "folder (R4QP01)")
    write(folder / "readme.txt", "not an image")
    write(folder / "TEX1_8x8_bbbbbbbbbbbbbbbb_14.png", "wrong prefix case")

    with zipfile.ZipFile(packs / "Zip Pack.zip", "w") as archive:
        archive.writestr("R4Q/", b"")
        archive.writestr(f"R4Q/Stages/{A}.png", data("zip A"), zipfile.ZIP_DEFLATED)
        archive.writestr(f"R4Q/Stages/{A}_mip1.png", data("zip A mip1"), zipfile.ZIP_STORED)
        archive.writestr(f"R4Q/{B}.PNG", data("zip B"), zipfile.ZIP_STORED)
        archive.writestr("R4Q/desktop.ini", b"[LocalizedFileNames]\n")
        # Written by a Windows tool: backslashes.
        archive.writestr(zipfile.ZipInfo(f"R4Q\\Fonts\\{C}.png"), data("zip C"))
        archive.writestr(f"R4Q/R4QP01/{C}.png", data("zip C (R4QP01)"))

    # zip64 records for every entry and the archive (more than 65535 entries).
    with zipfile.ZipFile(packs / "Big Pack.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        with archive.open(f"{A}.png", "w", force_zip64=True) as entry:
            entry.write(data("zip64 A"))
        for n in range(66000):
            archive.writestr(f"filler/{n}.txt", b"")

    with zipfile.ZipFile(packs / "notes.zip", "w") as archive:
        archive.writestr("notes.txt", b"no images here")
    (packs / "broken.zip").write_bytes(b"PK\x03\x04 this is not a zip archive")
    # The central directory is fine, the stored data is not.
    damaged = packs / "Damaged Data.zip"
    with zipfile.ZipFile(damaged, "w", zipfile.ZIP_STORED) as archive:
        archive.writestr(f"{A}.png", data("damaged A"))
    raw = bytearray(damaged.read_bytes())
    raw[30 + len(f"{A}.png") + 10] ^= 0xFF  # inside the entry's data
    damaged.write_bytes(bytes(raw))
    write(packs / "GALE01" / f"{A}.png", "another game's folder")
    write(packs / ".Hidden Pack" / f"{A}.png", "a pack still being copied in")
    (packs / "Empty Folder").mkdir(parents=True)
    write(packs / "Docs" / "guide.txt", "no images")

    write(out / "R4Q" / "Items" / f"{A}.png", "R4Q A")
    write(out / "loose" / f"{A}.png", "loose A")
    write(out / "loose" / "sub" / f"{B}.png", "loose B")

    return subprocess.call([test, str(out)])


if __name__ == "__main__":
    sys.exit(main())
