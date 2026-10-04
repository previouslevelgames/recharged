"""Synthetic disc checks for the public two-NIS-camera scene entry path."""
from pathlib import Path
import subprocess
import sys
import tempfile
from camera_fixture import camera_fixture
from disc_fixture import write_disc
from scene_fixture import make_assets

exe=Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="mscharged-pip-") as directory:
    root=Path(directory);disc=root/"disc.iso";config=root/"settings.ini"
    model,texture=make_assets()
    write_disc(disc,files={"model.rlg":model,"model.rlt":texture,
                           "primary.nis":camera_fixture(preview=True),"secondary.nis":camera_fixture(preview=True),
                           "bad.nis":camera_fixture()[:-4]})
    config.write_text("; Keep personal settings\n[game]\ndisc=disc.iso\n")
    before=config.read_bytes()
    base=[str(exe),"--experimental-scene","--config",str(config),"--frames","30",
          "--model","/model.rlg","--textures","/model.rlt"]
    pair=["--nis-primary","/primary.nis","--nis-secondary","/secondary.nis"]
    def run(args,code,message):
        result=subprocess.run(base+args,capture_output=True,text=True,timeout=35)
        output=result.stdout+result.stderr
        assert result.returncode==code and message in output,output
        assert "VUID-" not in output and "Validation Error" not in output,output
        assert config.read_bytes()==before
        if code==0:assert "Original graphics shutdown recovered both game arenas." in output,output
        print(message)
    run(pair,0,"Original NIS PIP: two retained authored cameras")
    run(pair+["--pip-expand",".25"],0,"Static preview rendered: 30 frames")
    run(["--nis-primary","/primary.nis"],2,"requires both")
    run(pair+["--camera","/primary.nis"],2,"cannot be combined")
    run(pair+["--pip-expand","nan"],2,"PIP expansion must")
    run(pair+["--nis-secondary","/missing.nis"],1,"Cannot open static asset")
    run(pair+["--nis-secondary","/bad.nis"],1,"chunk exceeds its container")
print("Two-NIS scene path, invalid data and unchanged configuration checks passed")
