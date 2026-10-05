"""Real NL NAV lifecycle on independently generated Wii data."""
from pathlib import Path
import subprocess,sys,tempfile
from disc_fixture import write_disc
from frontend_navigation_transition_fixture import files
with tempfile.TemporaryDirectory(prefix='charged-navigation-') as folder:
    root=Path(folder);write_disc(root/'navigation.iso',files=files(),fst_capacity=0x1000,partition_size=0x40000)
    subprocess.run([str(Path(sys.argv[1]).resolve()),str(root/'navigation.iso'),str(root),'generated'],check=True,timeout=80)
