"""Generated source15 hierarchy through real NL/SDL services."""
from pathlib import Path
import subprocess,sys,tempfile,os
from disc_fixture import write_disc
from frontend_visual_options_fixture import files
with tempfile.TemporaryDirectory(prefix='charged-visual-options-') as folder:
    root=Path(folder)
    write_disc(root/'visual-options.iso',files=files(),fst_capacity=0x1000,partition_size=0x40000)
    subprocess.run([str(Path(sys.argv[1]).resolve()),str(root/'visual-options.iso'),str(root),'generated'],check=True,timeout=90,
        env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy'})
