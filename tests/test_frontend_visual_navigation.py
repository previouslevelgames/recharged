"""Actual scene15/NAV flow with independent overlap order and real services."""
from pathlib import Path
import subprocess,sys,tempfile,os
from disc_fixture import write_disc
from frontend_visual_navigation_fixture import files
with tempfile.TemporaryDirectory(prefix='charged-visual-nav-') as folder:
    root=Path(folder)
    for mode in ('generated','overlap'):
        write_disc(root/(mode+'.iso'),files=files(mode=='overlap'),fst_capacity=0x1000,partition_size=0x40000)
        subprocess.run([str(Path(sys.argv[1]).resolve()),str(root/(mode+'.iso')),str(root),mode],check=True,timeout=90,
            env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy'})
