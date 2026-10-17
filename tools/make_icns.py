#!/usr/bin/env python3
"""Write assets/launcher/icon.icns (the macOS app icon) from assets/launcher/icon.png.

Each smaller size is a PNG made with ImageMagick (magick); the .icns file only
lists them. 512 pixels is the largest and the source PNG itself.
"""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# ICNS entry types holding PNG data, by pixel size. macOS picks the nearest
# size for Retina screens, so the @2x types that would repeat a size are left out.
ENTRIES = (("icp4", 16), ("icp5", 32), ("ic12", 64), ("ic07", 128), ("ic08", 256), ("ic09", 512))


def main():
    root = Path(__file__).resolve().parent.parent / "assets/launcher"
    source, target = root / "icon.png", root / "icon.icns"
    chunks = []
    with tempfile.TemporaryDirectory() as temporary:
        for kind, size in ENTRIES:
            png = Path(temporary) / f"{size}.png"
            if size == 512:
                png = source  # the source size, unchanged
            else:
                subprocess.run(["magick", str(source), "-resize", f"{size}x{size}", "-strip",
                                "-define", "png:compression-level=9", f"PNG:{png}"], check=True)
            data = png.read_bytes()
            chunks.append(kind.encode() + struct.pack(">I", len(data) + 8) + data)
    body = b"".join(chunks)
    target.write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + body)
    print(f"{target}: {len(body) + 8} bytes")


if __name__ == "__main__":
    sys.exit(main())
