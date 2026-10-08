"""Run whole AXInit and original OS stop at actual source protocol boundaries."""
from pathlib import Path
import hashlib
import subprocess
import sys

if len(sys.argv) not in (4, 5):
    raise SystemExit("usage: run_native_os_audio_functional_stop.py EXECUTABLE AX_IMAGE OS_IMAGE [ready|size|sync|yield]")
executable, ax_image, os_image = (Path(arg).resolve() for arg in sys.argv[1:4])
phases = (sys.argv[4],) if len(sys.argv) == 5 else ("ready", "size", "sync", "yield")
if any(phase not in ("ready", "size", "sync", "yield") for phase in phases):
    raise SystemExit("unknown source hardware phase")
identity = hashlib.sha256(ax_image.read_bytes()).hexdigest()
for phase in phases:
    result = subprocess.call([str(executable), str(ax_image), identity, str(os_image), phase])
    if result:
        raise SystemExit(result)
