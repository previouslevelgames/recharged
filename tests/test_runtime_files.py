#!/usr/bin/env python3
"""Exercise the original native NL file APIs with synthetic Wii data only."""
from pathlib import Path
import subprocess
import sys
import tempfile

from disc_fixture import write_disc


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="mscharged-nl-files-") as folder:
        root = Path(folder)
        disc = root / "synthetic files.iso"
        write_disc(disc, files={
            "empty.bin": b"",
            "large.bin": bytes((index * 37 + 11) & 255 for index in range(4097)),
            "small.txt": b"Synthetic fixture data.\n",
            "folder/end.bin": bytes([0x81, 0x23, 0x45]),
            "ini/common.ini": b"; Synthetic only\n[test]\nvalue = 7\n",
            "ini/datetime.ini": b"; Synthetic only\n[build]\ndate = fixture\n",
        })
        return subprocess.run([str(executable), str(disc), str(root)], timeout=40).returncode


if __name__ == "__main__":
    sys.exit(main())
