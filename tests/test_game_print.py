"""Ensure original call sites retain visible native diagnostics."""
from pathlib import Path
import subprocess
import sys

result = subprocess.run([str(Path(sys.argv[1]).resolve())], capture_output=True,
                        text=True, timeout=5)
assert result.returncode == 0, (result.returncode, result.stderr)
assert result.stdout == "", result.stdout
assert result.stderr == "Native diagnostics: camera 37 0x2a\n", result.stderr
