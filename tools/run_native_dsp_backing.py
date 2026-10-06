#!/usr/bin/env python3
"""Run the bounded original static-data qualifier with its actual image identity."""
from pathlib import Path
import hashlib
import subprocess
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: run_native_dsp_backing.py EXECUTABLE SOURCE_DATA_MODULE")
module = Path(sys.argv[2]).resolve()
identity = hashlib.sha256(module.read_bytes()).hexdigest()
raise SystemExit(subprocess.run([sys.argv[1], str(module), identity], timeout=25).returncode)
