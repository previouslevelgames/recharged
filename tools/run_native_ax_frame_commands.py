#!/usr/bin/env python3
"""Run actual source AX/AUX/AXFX command conformance; no game audio claim."""
import hashlib
from pathlib import Path
import subprocess,sys
if len(sys.argv)!=4:raise SystemExit('usage: run_native_ax_frame_commands.py EXECUTABLE SOURCE_IMAGE ORACLE')
executable,image,oracle=[Path(p).resolve()for p in sys.argv[1:]]
if not all(p.is_file()for p in [executable,image,oracle]):raise SystemExit('AX source/executable/oracle absent')
identity=hashlib.sha256(image.read_bytes()).hexdigest()
try:r=subprocess.run([str(executable),str(image),identity,str(oracle)],timeout=30)
except subprocess.TimeoutExpired:raise SystemExit('AX command/AUX qualifier timed out')
raise SystemExit(r.returncode if r.returncode>=0 else 1)
