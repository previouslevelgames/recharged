"""Real Vulkan integration with a wholly synthetic Wii disc and static assets."""
import pathlib
import subprocess
import sys
import tempfile

from disc_fixture import write_disc
from camera_fixture import camera_fixture
from scene_fixture import make_assets, make_shadow, make_world, make_specular_world, make_scrolling_specular_world, make_camera_overlay_world, make_masked_detail_world, make_scrolling_masked_detail_world, make_scrolling_camera_world, animate_texture_bundle

executable = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="mscharged-scene-") as directory:
    root = pathlib.Path(directory)
    model, texture = make_assets()
    disc, config = root / "disc.iso", root / "settings.ini"
    write_disc(disc, files={"scene.rlg": model, "scene.rlt": texture})
    config.write_text("; Preserve this personal file\n[game]\ndisc = disc.iso\nlanguage = french\n")
    before = config.read_bytes()
    base = [str(executable), "--experimental-scene", "--config", str(config), "--frames", "30"]

    def run(extra, code, text):
        result = subprocess.run(base + extra, text=True, capture_output=True, timeout=45)
        output = result.stdout + result.stderr
        assert result.returncode == code and text in output, output
        assert "VUID-" not in output and "Validation Error" not in output, output
        assert config.read_bytes() == before
        print(text)

    assets = ["--model", "/scene.rlg", "--textures", "/scene.rlt"]
    run(assets, 0, "Static preview rendered: 30 frames")
    write_disc(disc, files={"scene.rlg": model, "scene.rlt": texture, "camera.cam": camera_fixture(preview=True)})
    run(assets + ["--camera", "/camera.cam"], 0, "Original authored camera playback: /camera.cam")
    run(assets + ["--camera", "/missing.cam"], 1, "Camera asset is missing")
    write_disc(disc, files={"scene.rlg": model, "scene.rlt": texture, "camera.cam": camera_fixture()[:-4]})
    run(assets + ["--camera", "/camera.cam"], 1, "chunk exceeds its container")
    write_disc(disc, files={"scene.rlg": model, "scene.rlt": animate_texture_bundle(texture)})
    run(assets, 0, "2 textures, 1 texture animations; original radius")
    write_disc(disc, files={"scene.rlg": model, "scene.rlt": animate_texture_bundle(texture, missing_frame=True)})
    run(assets, 1, "animation frame texture is missing")
    write_disc(disc, files={"scene.rlg": model, "scene.rlt": texture})
    run(assets + ["--shadow-id", "5a5a5a5a"], 2, "must be supplied together")
    run(assets + ["--unlit"], 0, "Unlit comparison selected")
    lit_model, lit_texture = make_assets(lit=True)
    write_disc(disc, files={"scene.rlg": lit_model, "scene.rlt": lit_texture, "shadow.rlt": make_shadow()})
    shadow_args = ["--shadow-textures", "/shadow.rlt", "--shadow-id", "5a5a5a5a"]
    run(assets + shadow_args, 0, "Loaded original projected-shadow lookup: 8x4")
    run(assets + ["--shadow-textures", "/scene.rlt", "--shadow-id", "12345678"], 1, "requires a CI8/RGB5A3 texture")
    write_disc(disc, files={"scene.rlg": model, "scene.rlt": texture})
    run(assets + ["--model-id", "deadbeef"], 1, "Requested model ID is absent")
    run(["--model", "/missing.rlg", "--textures", "/scene.rlt"], 1, "Cannot open static asset")
    world = make_world()
    write_disc(disc, files={"world.tmp.zlib": world})
    run(["--world", "/world.tmp.zlib"], 2, "requires an explicit --model-id")
    run(["--world", "/world.tmp.zlib", "--model-id", "87654321", "--model", "/scene.rlg"], 2, "Select --world or separate")
    world_args = ["--world", "/world.tmp.zlib", "--model-id", "87654321"]
    run(world_args, 0, "Original stadium shadow blend samples: 2.")
    run(world_args + ["--camera", "/camera.cam"], 1, "Authored camera playback is not connected to the diagnostic shadow receiver")
    corrupted = bytearray(world); corrupted[-1] ^= 1
    write_disc(disc, files={"world.tmp.zlib": corrupted})
    run(world_args, 1, "Invalid compressed asset")
    write_disc(disc, files={"world.tmp.zlib": make_specular_world()})
    run(world_args, 0, "4 textures, 0 texture animations; original radius")
    write_disc(disc, files={"world.tmp.zlib": make_specular_world(missing_gloss=True)})
    run(world_args, 1, "Requested texture")
    write_disc(disc, files={"world.tmp.zlib": make_scrolling_specular_world()})
    run(world_args, 0, "2 textures, 0 texture animations; original radius")
    write_disc(disc, files={"world.tmp.zlib": make_scrolling_specular_world(missing_specular=True)})
    run(world_args, 1, "Requested texture")
    write_disc(disc, files={"world.tmp.zlib": make_camera_overlay_world()})
    run(world_args, 0, "3 textures, 0 texture animations; original radius")
    write_disc(disc, files={"world.tmp.zlib": make_camera_overlay_world(), "camera.cam": camera_fixture(preview=True)})
    run(world_args + ["--camera", "/camera.cam"], 0, "Original authored camera playback: /camera.cam")
    write_disc(disc, files={"world.tmp.zlib": make_camera_overlay_world(missing_mask=True)})
    run(world_args, 1, "Requested texture")
    write_disc(disc, files={"world.tmp.zlib": make_masked_detail_world()})
    run(world_args, 0, "3 textures, 0 texture animations; original radius")
    write_disc(disc, files={"world.tmp.zlib": make_masked_detail_world(missing_mask=True)})
    run(world_args, 1, "Requested texture")
    write_disc(disc, files={"world.tmp.zlib": make_scrolling_masked_detail_world()})
    run(world_args, 0, "3 textures, 0 texture animations; original radius")
    write_disc(disc, files={"world.tmp.zlib": make_scrolling_masked_detail_world(missing_mask=True)})
    run(world_args, 1, "Requested texture")
    write_disc(disc, files={"world.tmp.zlib": make_scrolling_masked_detail_world(animated=True)})
    run(world_args, 0, "4 textures, 1 texture animations; original radius")
    write_disc(disc, files={"world.tmp.zlib": make_scrolling_masked_detail_world(animated=True, missing_frame=True)})
    run(world_args, 1, "animation frame texture is missing")
    write_disc(disc, files={"world.tmp.zlib": make_scrolling_camera_world()})
    run(world_args, 0, "3 textures, 0 texture animations; original radius")
    write_disc(disc, files={"world.tmp.zlib": make_scrolling_camera_world(), "camera.cam": camera_fixture(preview=True)})
    run(world_args + ["--camera", "/camera.cam"], 0, "Original authored camera playback: /camera.cam")
    write_disc(disc, files={"world.tmp.zlib": make_scrolling_camera_world(missing_mask=True)})
    run(world_args, 1, "Requested texture")
    # Original alpha preparation disables depth writes for multibit textures.
    # The visibility gate must use actual colour samples for this valid case.
    blended = bytearray(texture)
    blended[43] = 8  # RLT header alpha bits
    for i in range(16):
        blended[64 + i * 2] = 128  # Tiled AR plane
    write_disc(disc, files={"scene.rlg": model, "scene.rlt": blended})
    run(assets, 0, "0 geometry depth samples")
    bad_texture = bytearray(texture)
    bad_texture[16:20] = bytes.fromhex("badc0ffe")
    write_disc(disc, files={"scene.rlg": model, "scene.rlt": bad_texture})
    run(assets, 1, "RLG diffuse texture is missing")
    run(assets + ["--page", "game"], 2, "Select one runtime mode")
    disc_data = bytearray(disc.read_bytes())
    disc_data[7] = 0
    disc.write_bytes(disc_data)
    run(assets, 1, "supports R4QE01 revision 1 only")
print("Synthetic Wii static preview GPU/error/unchanged-settings checks passed")
