"""Execute whole original font/text layout with independent synthetic data."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='charged original-textbox-') as directory:
    fixture = Path(directory) / 'fixture'
    subprocess.run([sys.executable, '-B', str(Path(__file__).with_name(
        'generate_original_textbox_fixture.py')), str(fixture)], check=True)
    result = subprocess.run([sys.argv[1], str(fixture / 'manifest.txt')],
        env={**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_RENDER_DRIVER': 'software', 'SDL_AUDIODRIVER': 'dummy'},
        capture_output=True, text=True, timeout=90)
    sys.stdout.write(result.stdout); sys.stderr.write(result.stderr)
    if result.returncode or 'actual original text layout checks=' not in result.stdout:
        raise SystemExit(result.returncode or 1)
