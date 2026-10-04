#!/usr/bin/env python3
"""Exercise ordered native NL audio reads on a synthetic Wii disc."""
from pathlib import Path
import subprocess
import sys
import tempfile
from disc_fixture import write_disc

executable = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="mscharged-audio-bank-") as folder:
    root = Path(folder)
    subprocess.run([executable, "--fixture", str(root)], check=True)
    metadata, wave = (root / "fixture.resbun").read_bytes(), (root / "fixture.nlxwb").read_bytes()
    for mode in ("success", "missing", "malformed", "short-wave"):
        files = {"audio/fixture.resbun": b"BAD!" + metadata[4:] if mode == "malformed" else metadata}
        if mode != "missing":
            files["audio/fixture.nlxwb"] = wave[:-1] if mode == "short-wave" else wave
        write_disc(root / "audio.iso", files=files)
        subprocess.run([executable, str(root / "audio.iso"), str(root), mode], check=True, timeout=25)
