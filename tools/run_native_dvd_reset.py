#!/usr/bin/env python3
"""Real native DVD reset gate using the existing synthetic Wii fixture only."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tests"))
from disc_fixture import write_disc

with tempfile.TemporaryDirectory(prefix="charged-dvd-reset-") as directory:
    path=Path(directory)/"reset.iso"
    write_disc(path,files={"payload.bin":bytes((i*37+11)&255 for i in range(1024))})
    raise SystemExit(subprocess.run([str(Path(sys.argv[1]).resolve()),str(path)],timeout=20).returncode)
