"""Run the bounded two-source-image native AX hardware qualifier."""
from pathlib import Path
import hashlib
import subprocess
import sys

if len(sys.argv) != 4:
    raise SystemExit("usage: run_native_ax_normal.py EXECUTABLE AX_IMAGE OS_IMAGE")
images = [Path(arg).resolve() for arg in sys.argv[2:]]
command = [str(Path(sys.argv[1]).resolve())]
for image in images:
    command.extend((str(image), hashlib.sha256(image.read_bytes()).hexdigest()))
raise SystemExit(subprocess.call(command))
