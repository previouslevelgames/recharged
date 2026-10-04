"""Generated rigid skin ownership/math checks; optional owned-disc audit."""
from pathlib import Path
import subprocess
import sys

executable = str(Path(sys.argv[1]).resolve())
subprocess.run([executable], check=True, timeout=50)
if len(sys.argv) == 4:
    subprocess.run([executable, '--owned', str(Path(sys.argv[2]).resolve()),
                    str(Path(sys.argv[3]).resolve())], check=True, timeout=180)
