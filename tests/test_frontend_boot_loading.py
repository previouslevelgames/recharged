"""Real NL retail-boot lifecycle over synthetic Wii data."""
from pathlib import Path
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from frontend_boot_loading_fixture import files

with tempfile.TemporaryDirectory(prefix='mscharged-frontend-boot-') as directory:
    root=Path(directory);write_disc(root/'boot.iso',files=files())
    subprocess.run([str(Path(sys.argv[1]).resolve()),str(root/'boot.iso'),str(root),'generated'],check=True,timeout=90)
