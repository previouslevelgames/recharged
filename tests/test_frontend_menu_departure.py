"""Genuine selected Main case6 over independent generated Wii assets."""
from pathlib import Path
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from frontend_main_menu_fixture import files as main_files
from frontend_options_fixture import files as options_files
from frontend_navigation_fixture import files as navigation_files
from frontend_camera_fixture import frontend_camera_files
from frontend_menu_transition_fixture import script

with tempfile.TemporaryDirectory(prefix='charged-menu-departure-') as folder:
    root = Path(folder)
    payloads = main_files()
    payloads.update(options_files())
    payloads.update(navigation_files())
    payloads.update(frontend_camera_files())
    payloads['Art/scripts/fe_presentation.byte_code'] = script()
    write_disc(root / 'menus.iso', files=payloads, fst_capacity=0x1000,
               partition_size=0x40000)
    subprocess.run([str(Path(sys.argv[1]).resolve()), str(root / 'menus.iso'),
                    str(root), 'generated'], check=True, timeout=110)
