from pathlib import Path
import os,subprocess,sys,tempfile
from disc_fixture import write_disc
from frontend_movie_image_fixture import files
with tempfile.TemporaryDirectory(prefix='charged-movie-image-') as folder:
    root=Path(folder);write_disc(root/'movie.iso',files=files(),fst_capacity=0x1000,partition_size=0x60000)
    subprocess.run([str(Path(sys.argv[1]).resolve()),str(root/'movie.iso'),'generated'],check=True,timeout=60,
        env={**os.environ,'SDL_AUDIODRIVER':'dummy'})
