"""Generated Audio scene14 through original stack/base ownership and host preferences."""
from pathlib import Path
import subprocess,sys,tempfile
from disc_fixture import write_disc
from frontend_audio_options_fixture import files
with tempfile.TemporaryDirectory(prefix='charged-audio-stack-') as directory:
    root=Path(directory);write_disc(root/'audio.iso',files=files(),fst_capacity=0x1000,partition_size=0x40000)
    subprocess.run([str(Path(sys.argv[1]).resolve()),str(root/'audio.iso'),str(root),'generated'],check=True,timeout=100)
