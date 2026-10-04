"""Run generated pose math/ownership tests; optionally audit private hierarchies."""
from pathlib import Path
import subprocess
import sys

executable=str(Path(sys.argv[1]).resolve())
subprocess.run([executable],check=True,timeout=50)
if len(sys.argv)==3:
    files=sorted(Path(sys.argv[2]).rglob('*.shier'))
    if not files:raise RuntimeError('No private hierarchy inputs found')
    subprocess.run([executable,'--owned',*[str(p) for p in files]],check=True,timeout=120)
