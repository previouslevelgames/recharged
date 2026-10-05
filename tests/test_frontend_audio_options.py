"""Synthetic source Audio submenu and real NL/SDL category playback."""
from pathlib import Path
import subprocess,sys,tempfile,os
from disc_fixture import write_disc
from frontend_audio_options_fixture import files
exe=str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='charged-audio-options-') as folder:
    root=Path(folder)
    subprocess.run([exe,'--fixture',str(root)],check=True)
    subprocess.run([exe,'--gain-disk',str(root)],check=True,timeout=10,
                   env={**os.environ,'SDL_AUDIODRIVER':'disk'})
    resources=files()
    resources['audio/FE_GEN_Music.resbun']=(root/'music.resbun').read_bytes()
    resources['audio/FE_GEN_Music.nlxwb']=(root/'music.nlxwb').read_bytes()
    write_disc(root/'audio-options.iso',files=resources,fst_capacity=0x1000,partition_size=0x40000)
    subprocess.run([exe,str(root/'audio-options.iso'),str(root),'generated'],check=True,timeout=90,env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy'})
