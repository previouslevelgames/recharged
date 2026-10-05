"""Original Title source through real NL resources and retained SDL audio."""
from pathlib import Path
import subprocess,sys,tempfile,os
from disc_fixture import write_disc
from frontend_title_fixture import files
exe=str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='charged-title-') as folder:
    root=Path(folder)
    subprocess.run([exe,'--fixture',str(root)],check=True,timeout=20)
    for mode in ('generated','missing'):
        contents=files()
        contents.update({'audio/calculation.bun':(root/'calculation.bun').read_bytes(),
            'audio/FE_GEN_Music.resbun':(root/'music.resbun').read_bytes(),
            'audio/FE_GEN_Music.nlxwb':(root/'music.nlxwb').read_bytes()})
        if mode=='missing':del contents['Art/fe/sms2_start.fen']
        disc=root/(mode+'.iso')
        write_disc(disc,files=contents,fst_capacity=0x1000,partition_size=0x60000)
        subprocess.run([exe,str(disc),str(root),mode],check=True,timeout=90,
            env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_RENDER_DRIVER':'software','SDL_AUDIODRIVER':'dummy'})
