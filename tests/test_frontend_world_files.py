"""Original FE paths, sequential compressed reads and explicit static coverage."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from disc_fixture import write_disc
from world_scene_fixture import world_scene_fixture

executable = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="mscharged-frontend-world-") as folder:
    root = Path(folder)
    for mode in ("success", "partial", "missing-model", "missing-res", "missing-tmp", "bad-res", "bad-tmp", "bad-ini"):
        resident, temporary = world_scene_fixture(missing_model=mode == "missing-model")
        if mode == "partial":
            resident = bytearray(resident)
            struct.pack_into(">I", resident, 32 + 8, 0x108)  # Known unsupported type with same stride.
        def compressed(data):
            return struct.pack(">I", len(data)) + zlib.compress(data)
        res = "Art/fe/environments/main/gameworld.res.zlib"
        tmp = "Art/fe/environments/main/gameworld.tmp.zlib"
        ini = "ini/Stadiums/FEWorld.ini"
        files = {res: compressed(resident), tmp: compressed(temporary), ini: b"[Render/Fog]\nFog Enabled = false\n"}
        if mode.startswith("missing-") and mode != "missing-model":
            del files[res if mode == "missing-res" else tmp]
        elif mode.startswith("bad-"):
            files[{"bad-res": res, "bad-tmp": tmp, "bad-ini": ini}[mode]] = b"broken\x00data"
        write_disc(root / "world.iso", files=files, fst_capacity=0x400)
        result = subprocess.run([executable, str(root / "world.iso"), str(root), mode], timeout=20)
        if result.returncode:
            sys.exit(result.returncode)
