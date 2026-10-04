#!/usr/bin/env python3
"""Real NL async batch ownership and compressed-file failure checks."""
from pathlib import Path
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from particle_files_fixture import PATHS, particle_files_fixture

executable = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="mscharged-particle-files-") as folder:
    root = Path(folder)
    for mode in ("success", "compressed-short", "compressed-checksum", "compressed-size", "compressed-trailing",
                 *(f"missing-{i}" for i in range(4)), *(f"empty-{i}" for i in range(4))):
        files = particle_files_fixture()
        if mode.startswith("missing-"):
            del files[PATHS[int(mode[-1])]]
        elif mode.startswith("empty-"):
            files[PATHS[int(mode[-1])]] = b""
        elif mode == "compressed-short":
            files[PATHS[1]] = b"\0\1"
        elif mode == "compressed-checksum":
            files[PATHS[1]] = files[PATHS[1]][:-1] + bytes([files[PATHS[1]][-1] ^ 1])
        elif mode == "compressed-size":
            files[PATHS[1]] = b"\x7f\xff\xff\xff" + files[PATHS[1]][4:]
        elif mode == "compressed-trailing":
            files[PATHS[1]] += b"!"
        write_disc(root / "particle.iso", files=files)
        result = subprocess.run([executable, str(root / "particle.iso"), str(root), mode], timeout=20)
        if result.returncode:
            sys.exit(result.returncode)
