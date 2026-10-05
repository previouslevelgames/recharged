from pathlib import Path
import subprocess,sys,tempfile,os
from disc_fixture import write_disc
from frontend_credits_fixture import files
with tempfile.TemporaryDirectory(prefix='charged-credits-stack-') as folder:
    root=Path(folder)
    for mode in ('generated','missing'):
        contents=files()
        if mode=='missing':del contents['Art/fe/credits.fen']
        write_disc(root/(mode+'.iso'),files=contents,fst_capacity=0x1000,partition_size=0x60000)
        subprocess.run([str(Path(sys.argv[1]).resolve()),str(root/(mode+'.iso')),str(root),mode],check=True,timeout=90,
            env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy'})
