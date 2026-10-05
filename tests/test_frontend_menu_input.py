"""Composed original Options/NAV over independent generated Wii data."""
from pathlib import Path
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from frontend_options_fixture import files as options_files
from frontend_navigation_fixture import files as navigation_files

with tempfile.TemporaryDirectory(prefix='charged-options-navigation-') as folder:
    root = Path(folder)
    payloads = options_files()
    payloads.update(navigation_files())
    write_disc(root / 'navigation.iso', files=payloads, fst_capacity=0x1000, partition_size=0x40000)
    subprocess.run([str(Path(sys.argv[1]).resolve()), str(root / 'navigation.iso'),
                    str(root), 'generated'], check=True, timeout=100)
