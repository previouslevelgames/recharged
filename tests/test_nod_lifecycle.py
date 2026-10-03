#!/usr/bin/env python3
"""Native nod worker ownership over an original generated ISO, without SDL."""
from pathlib import Path
import subprocess
import sys
import tempfile
from disc_fixture import write_disc

with tempfile.TemporaryDirectory(prefix="mscharged-nod-lifecycle-") as folder:
    iso=Path(folder)/"lifecycle.iso"
    write_disc(iso)
    # Four full raw sector groups permit requested and read-ahead I/O to be
    # controlled independently. Every byte is generated here, never game data.
    with iso.open("ab") as output:
        output.write(bytes(4*64*32768-iso.stat().st_size))
    subprocess.run([str(Path(sys.argv[1]).resolve()),str(iso),*sys.argv[2:]],check=True,timeout=15)
