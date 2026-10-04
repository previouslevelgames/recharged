"""Actual NL-backed handler/session lifecycle over generated Wii data."""
from pathlib import Path
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from frontend_handler_fixture import files

with tempfile.TemporaryDirectory(prefix='mscharged-frontend-handler-') as directory:
    root=Path(directory);write_disc(root/'handler.iso',files=files())
    subprocess.run([str(Path(sys.argv[1]).resolve()),str(root/'handler.iso'),str(root),'generated'],check=True,timeout=90)
