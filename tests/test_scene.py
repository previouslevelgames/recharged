"""Real Vulkan integration with a wholly synthetic Wii disc and static assets."""
import pathlib
import subprocess
import sys
import tempfile

from disc_fixture import write_disc
from scene_fixture import make_assets

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
    run(assets + ["--model-id", "deadbeef"], 1, "Requested model ID is absent")
    run(["--model", "/missing.rlg", "--textures", "/scene.rlt"], 1, "Cannot open static asset")
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
