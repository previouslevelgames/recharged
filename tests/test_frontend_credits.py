"""Original Credits prefix and kernels through actual NL/SDL services."""
from pathlib import Path
import subprocess,sys,tempfile,os
from disc_fixture import write_disc
from frontend_credits_fixture import files
with tempfile.TemporaryDirectory(prefix='charged-credits-') as folder:
    root=Path(folder)
    write_disc(root/'credits.iso',files=files(),fst_capacity=0x1000,partition_size=0x60000)
    subprocess.run([str(Path(sys.argv[1]).resolve()),str(root/'credits.iso'),str(root),'generated'],check=True,timeout=90,
        env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy'})
