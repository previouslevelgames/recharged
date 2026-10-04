#!/usr/bin/env python3
"""Validate real NL boot-script reads against synthetic Wii images."""
from pathlib import Path
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from boot_script_fixture import boot_script_fixture

executable = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="mscharged-boot-script-") as folder:
    root = Path(folder)
    for mode in ("success", "missing", "empty", "malformed", "truncated"):
        data = boot_script_fixture()
        if mode == "empty":
            data = b""
        elif mode == "malformed":
            data = b"BAD!" + data[4:]
        elif mode == "truncated":
            data = data[:-1]
        files = {} if mode == "missing" else {"Art/scripts/async_loading.byte_code": data}
        write_disc(root / "boot.iso", files=files)
        result = subprocess.run([executable, str(root / "boot.iso"), str(root), mode], timeout=20)
        if result.returncode:
            sys.exit(result.returncode)
