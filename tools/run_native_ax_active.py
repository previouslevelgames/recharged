#!/usr/bin/env python3
"""Run selected-source AX/PB arithmetic with deliberate synthetic DROM only."""
import hashlib
from pathlib import Path
import subprocess
import sys

def main():
    if len(sys.argv)!=4:
        print("usage: run_native_ax_active.py EXECUTABLE SOURCE_IMAGE ORACLE_DIRECTORY",file=sys.stderr);return 2
    executable,image,directory=[Path(p).resolve()for p in sys.argv[1:]]
    files=[directory/p for p in("oracle.bin","synthetic-drom.bin","mix-oracle.bin")]
    if not all(p.is_file()for p in[executable,image,*files]):
        print("AX qualifier source/executable/vectors absent",file=sys.stderr);return 2
    identity=hashlib.sha256(image.read_bytes()).hexdigest()
    try:result=subprocess.run([str(executable),str(image),identity,*map(str,files)],timeout=30)
    except subprocess.TimeoutExpired:
        print("AX active PB qualifier timed out",file=sys.stderr);return 1
    return result.returncode if result.returncode>=0 else 1
if __name__=="__main__":sys.exit(main())
