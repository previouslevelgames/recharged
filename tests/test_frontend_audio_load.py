#!/usr/bin/env python3
"""Real native NL FE bank reads followed by retained cue output; no game bytes."""
from pathlib import Path
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
fixture, executable = map(lambda p: str(Path(p).resolve()), sys.argv[1:])
with tempfile.TemporaryDirectory(prefix="charged-fe-audio-") as directory:
    root = Path(directory)
    subprocess.run([fixture, "--fixture", str(root)], check=True)
    metadata = (root / "FE_GEN_Sfx.resbun").read_bytes()
    wave = (root / "FE_GEN_Sfx.nlxwb").read_bytes()
    calculation = (root / "calculation.bun").read_bytes()
    for mode in ("success", "missing", "malformed", "cancel"):
        files = {"audio/calculation.bun": calculation,
                 "audio/FE_GEN_Sfx.resbun": b"BAD!" + metadata[4:] if mode == "malformed" else metadata}
        if mode != "missing":
            files["audio/FE_GEN_Sfx.nlxwb"] = wave
        write_disc(root / "frontend-audio.iso", files=files)
        subprocess.run([executable, str(root / "frontend-audio.iso"), str(root), mode],
                       check=True, timeout=30)
