"""Source Credits phase/abort/scroll ownership with real NL and SDL services."""
from pathlib import Path
import os,sys,tempfile,subprocess
from disc_fixture import write_disc
from frontend_credits_playback_fixture import files,scene
from thp_movie_fixture import movie

def payloads():
    result=files()
    result['Art/movies/nlgintrowide.thp']=movie()
    result['Art/movies/credits.thp']=movie()
    return result

def main():
    with tempfile.TemporaryDirectory(prefix='charged-credits-phases-') as tmp:
        root=Path(tmp)
        for mode in ('generated','missing','malformed','empty'):
            data=payloads()
            if mode=='empty':data['credits.txt']=b'# no credit lines\n';data['Art/fe/credits.fen']=scene(True)
            if mode=='missing': del data['credits.txt']
            elif mode=='malformed':data['credits.txt']=b'A\0B\n'
            write_disc(root/f'{mode}.iso',files=data,fst_capacity=0x1000,partition_size=0x60000)
            subprocess.run([str(Path(sys.argv[1]).resolve()),str(root/f'{mode}.iso'),str(root),mode],check=True,timeout=90,
                env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy'})
if __name__=='__main__':main()
