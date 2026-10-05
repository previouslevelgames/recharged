"""Original Main callbacks over generated disc resources; no retail data."""
from pathlib import Path
import subprocess,sys,tempfile
from disc_fixture import write_disc
from frontend_main_menu_fixture import files

with tempfile.TemporaryDirectory(prefix='mscharged-main-menu-') as folder:
    root=Path(folder)
    write_disc(root/'main.iso',files=files(),fst_capacity=0x1000,partition_size=0x20000)
    subprocess.run([str(Path(sys.argv[1]).resolve()),str(root/'main.iso'),str(root),'generated'],check=True,timeout=80)
