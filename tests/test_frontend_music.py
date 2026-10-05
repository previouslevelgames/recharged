#!/usr/bin/env python3
"""Generated stereo IDSP through real NL refill and SDL output, no retail bytes."""
from pathlib import Path
import subprocess,sys,tempfile,os
from disc_fixture import write_disc
executable=str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='charged-music-') as directory:
    root=Path(directory)
    subprocess.run([executable,'--fixture',str(root)],check=True)
    metadata=(root/'music.resbun').read_bytes();wave=(root/'music.nlxwb').read_bytes()
    for mode in ('success','finite','missing','malformed','badheader','badmain','cancel','disk'):
        chosen=(root/'finite.resbun').read_bytes() if mode in ('finite','disk') else metadata
        if mode=='malformed':chosen=b'BAD!'+chosen[4:]
        data=bytearray(wave)
        if mode=='badheader':data[0]=0
        if mode=='badmain':data[6368]=0
        files={'audio/calculation.bun':(root/'calculation.bun').read_bytes(),'audio/FE_GEN_Music.resbun':chosen}
        if mode!='missing':files['audio/FE_GEN_Music.nlxwb']=bytes(data)
        write_disc(root/'music.iso',files=files)
        subprocess.run([executable,str(root/'music.iso'),str(root),mode],check=True,timeout=20,env={**os.environ,"SDL_AUDIODRIVER":"disk" if mode=="disk" else "dummy"})
