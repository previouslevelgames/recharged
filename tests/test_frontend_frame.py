"""Static authored image/text selection through FEN reads, scheduled views and Vulkan."""
from pathlib import Path
import struct
import re
import subprocess
import sys
import tempfile
import zlib
from disc_fixture import write_disc
from camera_fixture import camera_fixture
from world_scene_fixture import world_scene_fixture
from frontend_visual_fixture import files, name_hash
from frontend_layout_fixture import layout
from frontend_animation_fixture import animation


def frame(with_image=False):
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
    if with_image:
        def append(blob):
            at = len(data)
            data.extend(blob)
            return at

        def word(at, value):
            struct.pack_into(">I", data, at, value)

        def pointer(at, value):
            word(at, value)
            if at not in relocations:
                relocations.append(at)

        image = append(data[0x80:0x118])
        library = append(data[0x1a0:0x21c])
        resource = append(bytearray(32))
        # One mixed instance ring; the image is submitted after both labels.
        pointer(0x38, image)
        pointer(0x80, second); pointer(0x84, image)
        pointer(second, image); pointer(second + 4, 0x80)
        pointer(image, 0x80); pointer(image + 4, second)
        pointer(image + 12, library)
        word(image + 0x88, 2)
        pointer(image + 0x90, resource); word(image + 0x94, 1)
        struct.pack_into(">3f", data, image + 0x3c, 0, 0, 0)
        # The original image's library supplies its UV rectangle.
        pointer(12, library)
        pointer(0x1a0, library); pointer(0x1a4, library)
        pointer(library, 0x1a0); pointer(library + 4, 0x1a0)
        word(library + 0x74, 1); pointer(library + 0x78, resource)
        struct.pack_into(">4f", data, library + 0x40, 0, 0, 1, 1)
        pointer(8, resource); word(20, 2)
        pointer(0x240, resource); pointer(0x244, resource)
        pointer(resource, 0x240); pointer(resource + 4, 0x240)
        word(resource + 8, 0); word(resource + 12, name_hash("frame-image"))
    table = b"".join(struct.pack(">I", value) for value in sorted(relocations))
    return struct.pack(">4I", 0x46454e4c, 1, len(data), len(table)) + data + table


def image_bundle():
    # A single synthetic red RGBA8 tile inside an original sector bundle.
    texture = bytearray(96)
    struct.pack_into(">2I", texture, 0, 1, 3)
    struct.pack_into(">2H", texture, 14, 4, 4)
    texture[32:64] = bytes([255, 255]) * 16  # Alpha/red plane.
    bundle = bytearray(64)
    struct.pack_into(">4I", bundle, 0, 32, 1, 1, 2)
    struct.pack_into(">3I", bundle, 32, name_hash("frame-image"), 2, len(texture))
    return bundle + texture


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

    def run(expected=0, message="Authored frontend frame rendered: 30 frames, 2 text components, 0 image components per frame.", extra=()):
        before = config.read_bytes()
        result = subprocess.run(base + list(extra), capture_output=True, text=True, timeout=40)
        output = result.stdout + result.stderr
        assert result.returncode == expected and message in output, (result.returncode, output)
        assert "VUID-" not in output and "Validation Error" not in output, output
        assert config.read_bytes() == before
        if expected == 0:
            assert "shutdown recovered both game arenas" in output, output
            assert "Mixed frontend text and images use retained registrations and original ordered GL packets." in output, output
        print(message)
        return output

    write_disc(disc, files=payloads, fst_capacity=0x800)
    run()
    run(extra=("--frontend-slide", "Slide"))
    run(1, "Frontend presentation slide name is absent", ("--frontend-slide", "missing"))
    run(2, "Select --frontend-frame or --frontend-layout", ("--frontend-layout", "/Art/fe/test.fen"))
    run(2, "--frontend-images requires --frontend-frame and either main or ingame", ("--frontend-images", "invalid"))
    no_frame = subprocess.run([executable, "--experimental-scene", "--frontend-images", "main"],
                              capture_output=True, text=True, timeout=10)
    assert no_frame.returncode == 2 and "requires --frontend-frame" in no_frame.stderr
    no_frame = subprocess.run([executable, "--experimental-scene", "--frontend-animate"],
                              capture_output=True, text=True, timeout=10)
    assert no_frame.returncode == 2 and "--frontend-animate requires --frontend-frame" in no_frame.stderr
    payloads["Art/fe/test.fen"] = frame(with_image=True)
    payloads["Art/fe/MainUI.Dmn"] = image_bundle()
    payloads["Art/fe/InGameUI.Res"] = struct.pack(">4I", 32, 0, 1, 1)
    payloads["Art/fe/InGameUI.Dmn"] = image_bundle()
    write_disc(disc, files=payloads, fst_capacity=0x800)
    mixed = "Authored frontend frame rendered: 30 frames, 2 text components, 1 image components per frame."
    run(message=mixed)
    run(message=mixed, extra=("--frontend-images", "ingame"))
    payloads["Art/fe/test.fen"] = animation()
    write_disc(disc, files=payloads, fst_capacity=0x800)
    animated = run(message="Original frontend timeline advanced: 30 updates; presentation time",
                   extra=("--frontend-animate",))
    time = float(re.search(r"presentation time ([0-9.]+) seconds", animated).group(1))
    assert abs(time - 29 / 60) < 1e-5, animated
    assert "0 text components, 1 image components in the last frame" in animated, animated
    payloads["Art/fe/test.fen"] = frame(with_image=True)
    payloads["Art/fe/MainUI.Dmn"] = struct.pack(">4I", 32, 0, 1, 1)
    write_disc(disc, files=payloads, fst_capacity=0x800)
    run(1, "is absent from the selected bundle profile")
    payloads["Art/fe/test.fen"] = b"invalid layout"
    write_disc(disc, files=payloads, fst_capacity=0x800)
    run(1, "FAILED:")
