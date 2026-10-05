"""Generated Main/Options source lifecycle; no game bytes."""
from pathlib import Path
import subprocess,sys,tempfile
from disc_fixture import write_disc
from frontend_main_menu_fixture import files as main_files
from frontend_options_fixture import files as options_files
with tempfile.TemporaryDirectory(prefix='charged-stack-visual-') as folder:
    root=Path(folder);files=main_files();files.update(options_files())
    write_disc(root/'visual.iso',files=files,fst_capacity=0x1000,partition_size=0x40000)
    subprocess.run([str(Path(sys.argv[1]).resolve()),str(root/'visual.iso'),str(root),'generated'],check=True,timeout=100)
