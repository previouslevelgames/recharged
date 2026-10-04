"""FEN + NLOC + font pages through original views and scheduled GX frames."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from disc_fixture import write_disc
from camera_fixture import camera_fixture
from world_scene_fixture import world_scene_fixture
from frontend_visual_fixture import files
from frontend_layout_fixture import layout

executable = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="mscharged-fe-layout-") as folder:
    root = Path(folder)
    disc, config = root / "scene.iso", root / "settings.ini"
    resident, temporary = world_scene_fixture(spacing=.3, depth_step=0)

    def compressed(data):
        return struct.pack(">I", len(data)) + zlib.compress(data)

    payloads = files()
    payloads.update({
        "Art/fe/environments/main/gameworld.res.zlib": compressed(resident),
        "Art/fe/environments/main/gameworld.tmp.zlib": compressed(temporary),
        "Art/fe/environments/cameras/start_idle.cam": camera_fixture(preview=True),
        "ini/Stadiums/FEWorld.ini": b"[Render/Fog]\nFog Enabled = false\n",
        "Art/fe/test.fen": layout(),
    })
    base = [executable, "--experimental-scene", "--frontend-world", "--frontend-layout", "/Art/fe/test.fen",
            "--config", str(config), "--frames", "30"]

    def run(expected=0, message="Frontend text inspection rendered: 30 frames", extra=()):
        before = config.read_bytes()
        result = subprocess.run(base + list(extra), capture_output=True, text=True, timeout=40)
        output = result.stdout + result.stderr
        assert result.returncode == expected and message in output, (result.returncode, output)
        assert "VUID-" not in output and "Validation Error" not in output, output
        assert config.read_bytes() == before
        if expected == 0:
            assert "shutdown recovered both game arenas" in output, output
            assert "Frontend text uses registered font pages and original GL packets." in output, output
            assert "1 stored text components" in output, output
        print(message)

    write_disc(disc, files=payloads, fst_capacity=0x800)
    for language in ("english", "french", "spanish"):
        config.write_text("; preserve me\n[game]\ndisc=scene.iso\nlanguage=" + language + "\n")
        run()
    run(2, "cannot be combined", ["--debug-camera"])
    payloads["Art/fe/test.fen"] = b"invalid layout"
    write_disc(disc, files=payloads, fst_capacity=0x800)
    run(1, "FAILED:")
    del payloads["Art/fe/test.fen"]
    write_disc(disc, files=payloads, fst_capacity=0x800)
    run(1, "Cannot open static asset")
