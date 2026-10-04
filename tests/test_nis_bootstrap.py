#!/usr/bin/env python3
"""Five real async reads, transactional publication and cancellation."""
from pathlib import Path
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from nis_bootstrap_fixture import bootstrap_files

executable = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="mscharged-nis-bootstrap-") as folder:
    root = Path(folder)
    cases = [("success", None), ("missing", None)] + [("malformed", key) for key in bootstrap_files()]
    for mode, malformed in cases:
        files = bootstrap_files()
        if mode == "missing":
            del files["Art/nis/nis_dict.txt"]
        elif mode == "malformed":
            files[malformed] = b"bad\x00data"  # Bytecode headers and all three text files reject this.
        write_disc(root / "bootstrap.iso", files=files, fst_capacity=0x400)
        result = subprocess.run([executable, str(root / "bootstrap.iso"), str(root), mode], timeout=20)
        if result.returncode:
            sys.exit(result.returncode)
