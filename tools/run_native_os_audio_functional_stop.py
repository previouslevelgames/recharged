"""Run genuine whole AXInit and original OS stop on the idle native device."""
from pathlib import Path
import hashlib
import subprocess
import sys

if len(sys.argv) != 4:
    raise SystemExit("usage: run_native_os_audio_functional_stop.py EXECUTABLE AX_IMAGE OS_IMAGE")
executable, ax_image, os_image = (Path(arg).resolve() for arg in sys.argv[1:])
raise SystemExit(subprocess.call([str(executable), str(ax_image),
    hashlib.sha256(ax_image.read_bytes()).hexdigest(), str(os_image)]))
