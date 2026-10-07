"""Run whole-source AX with the explicit functional-native hardware contract."""
from pathlib import Path
import hashlib
import subprocess
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: run_native_ax_functional.py EXECUTABLE AX_IMAGE")
image = Path(sys.argv[2]).resolve()
raise SystemExit(subprocess.call([str(Path(sys.argv[1]).resolve()), str(image),
                                hashlib.sha256(image.read_bytes()).hexdigest()]))
