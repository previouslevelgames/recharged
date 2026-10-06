"""Actual whole-source localization against independent synthetic Wii data."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="charged original-localization-") as temporary:
    fixture = Path(temporary) / "fixture"
    subprocess.run([sys.executable, "-B", str(Path(__file__).with_name(
        "generate_original_localization_fixture.py")), str(fixture)], check=True)
    result = subprocess.run([sys.argv[1], str(fixture / "manifest.txt")],
        env={**os.environ, "SDL_VIDEODRIVER": "dummy", "SDL_RENDER_DRIVER": "software",
             "SDL_AUDIODRIVER": "dummy"}, text=True, capture_output=True, timeout=60)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode or "actual original localization checks=" not in result.stdout:
        raise SystemExit(result.returncode or 1)
