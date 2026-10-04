"""Virtual SDL controller moves and resets the real DebugCam Vulkan preview."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from disc_fixture import write_disc
from world_scene_fixture import world_scene_fixture

with tempfile.TemporaryDirectory(prefix='mscharged-debug-input-') as directory:
    root = Path(directory)
    resident, temporary = world_scene_fixture(spacing=.3, depth_step=0)
    def compress(data): return struct.pack('>I',len(data))+zlib.compress(data)
    write_disc(root/'world.iso',files={'world.res.zlib':compress(resident),'world.tmp.zlib':compress(temporary)})
    config=root/'settings.ini';config.write_text('[game]\ndisc=world.iso\n');before=config.read_bytes()
    result=subprocess.run([str(Path(sys.argv[1]).resolve()),str(config)],capture_output=True,text=True,timeout=45)
    output=result.stdout+result.stderr
    if result.returncode == 77:
        print(output)
        sys.exit(77)
    assert result.returncode == 0, output
    assert 'Rendered SDL DebugCam: orbit=' in output and 'shutdown recovered both game arenas' in output, output
    assert 'Static preview rendered: 180 frames' in output, output
    assert 'VUID-' not in output and 'Validation Error' not in output, output
    assert config.read_bytes() == before
    print(output)
