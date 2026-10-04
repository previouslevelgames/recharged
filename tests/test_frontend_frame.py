"""Static authored text selection through FEN reads, scheduled views and Vulkan."""
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


def frame():
    single = layout()
    size = struct.unpack_from(">I", single, 8)[0]
    data = bytearray(single[16:16 + size])
    relocations = list(struct.unpack(">" + "I" * ((len(single) - 16 - size) // 4), single[16 + size:]))
    second = len(data)
    data.extend(data[0x80:0x1a0])
    for at, value in ((0x80, second), (0x84, second), (second, 0x80), (second + 4, 0x80)):
        struct.pack_into(">I", data, at, value)
    relocations += [second, second + 4, second + 12]
    data[second + 0x18:second + 0x20] = b"LabelTwo"
    for at, x in ((0x80, -100), (second, 100)):
        struct.pack_into(">2f", data, at + 0x98, 128, 64)
        struct.pack_into(">I", data, at + 0x84, 1)  # Override position.
        struct.pack_into(">3f", data, at + 0x3c, x, 0, 0)
    table = b"".join(struct.pack(">I", value) for value in sorted(relocations))
    return struct.pack(">4I", 0x46454e4c, 1, len(data), len(table)) + data + table


executable = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="mscharged-fe-frame-") as folder:
    root = Path(folder)
    disc, config = root / "scene.iso", root / "settings.ini"
    resident, temporary = world_scene_fixture(spacing=.3, depth_step=0)
    compressed = lambda data: struct.pack(">I", len(data)) + zlib.compress(data)
    payloads = files()
    payloads.update({
        "Art/fe/environments/main/gameworld.res.zlib": compressed(resident),
        "Art/fe/environments/main/gameworld.tmp.zlib": compressed(temporary),
        "Art/fe/environments/cameras/start_idle.cam": camera_fixture(preview=True),
        "ini/Stadiums/FEWorld.ini": b"[Render/Fog]\nFog Enabled = false\n",
        "Art/fe/test.fen": frame(),
    })
    config.write_text("; retain settings\n[game]\ndisc=scene.iso\nlanguage=english\n")
    base = [executable, "--experimental-scene", "--frontend-world", "--frontend-frame", "/Art/fe/test.fen",
            "--config", str(config), "--frames", "30"]

    def run(expected=0, message="Authored frontend frame rendered: 30 frames, 2 text components per frame.", extra=()):
        before = config.read_bytes()
        result = subprocess.run(base + list(extra), capture_output=True, text=True, timeout=40)
        output = result.stdout + result.stderr
        assert result.returncode == expected and message in output, (result.returncode, output)
        assert "VUID-" not in output and "Validation Error" not in output, output
        assert config.read_bytes() == before
        if expected == 0:
            assert "shutdown recovered both game arenas" in output, output
        print(message)

    write_disc(disc, files=payloads, fst_capacity=0x800)
    run()
    run(extra=("--frontend-slide", "Slide"))
    run(1, "Frontend presentation slide name is absent", ("--frontend-slide", "missing"))
    run(2, "Select --frontend-frame or --frontend-layout", ("--frontend-layout", "/Art/fe/test.fen"))
    payloads["Art/fe/test.fen"] = b"invalid layout"
    write_disc(disc, files=payloads, fst_capacity=0x800)
    run(1, "FAILED:")
