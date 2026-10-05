import os,sys,tempfile,subprocess
from pathlib import Path
from disc_fixture import write_disc
from thp_movie_fixture import files

def main():
    exe=Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='charged-movie-playback-') as tmp:
        root=Path(tmp);write_disc(root/'test.iso',files=files(),fst_capacity=0x1000)
        for driver in ['dummy','disk']:
            result=subprocess.run([str(exe),str(root/'test.iso'),str(root),'generated'],timeout=35,
                env={**os.environ,'SDL_AUDIODRIVER':driver,'SDL_VIDEODRIVER':'dummy','SDL_RENDER_DRIVER':'software'})
            if result.returncode:return result.returncode
    return 0
if __name__=='__main__':raise SystemExit(main())
