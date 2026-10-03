#!/usr/bin/env python3
"""Named camera transactions on a generated Wii ISO, without retail data."""
from pathlib import Path
import subprocess
import sys
import tempfile
from camera_fixture import camera_fixture
from disc_fixture import write_disc

with tempfile.TemporaryDirectory(prefix='mscharged-camera-batch-') as folder:
    root=Path(folder)
    camera=camera_fixture()
    (root/'camera.cam').write_bytes(camera)
    write_disc(root/'cameras.iso',files={
        'camera.cam':camera,
        'empty.cam':b'',
        'malformed.cam':b'not a camera',
        'truncated.cam':camera[:-1],
    })
    sys.exit(subprocess.run([str(Path(sys.argv[1]).resolve()),str(root/'cameras.iso'),str(root)],timeout=55).returncode)
