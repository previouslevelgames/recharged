"""Actual NL scene15 load, selected lifecycle and missing-source rollback."""
from pathlib import Path
import subprocess,sys,tempfile,os
from disc_fixture import write_disc
from frontend_visual_options_fixture import files
with tempfile.TemporaryDirectory(prefix='charged-visual-stack-') as folder:
    root=Path(folder)
    for mode in ('generated','missing'):
        payload=files()
        if mode=='missing': del payload['Art/fe/options_visual_options.fen']
        write_disc(root/(mode+'.iso'),files=payload,fst_capacity=0x1000,partition_size=0x40000)
        subprocess.run([str(Path(sys.argv[1]).resolve()),str(root/(mode+'.iso')),str(root),mode],check=True,timeout=90,
            env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy'})
