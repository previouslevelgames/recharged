"""Original particle loading/rendering through a synthetic Wii disc and Vulkan."""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import zlib

from disc_fixture import write_disc
from effects_registry_fixture import chunk, textures
from particle_files_fixture import PATHS
from scene_fixture import make_assets
from test_particle_simulation import resident


executable = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="mscharged-particle-preview-") as temporary:
    root = Path(temporary)
    disc, config = root / "scene.iso", root / "settings.ini"
    model, texture = make_assets()
    # Use the diagnostic's fixed group identity with wholly synthetic source
    # records; no game asset is needed by this integration check.
    effects = resident().replace(struct.pack(">I", 0x81f2a311), struct.pack(">I", 0xfda2d744))
    effects = effects.replace(struct.pack(">I", 0x12345678), struct.pack(">I", 0x13572468))
    nonresident = chunk(0x80000001, chunk(0x24100, textures(0x13572468)))
    payloads = {
        "scene.rlg": model,
        "scene.rlt": texture,
        PATHS[0]: effects,
        PATHS[1]: struct.pack(">I", len(nonresident)) + zlib.compress(nonresident),
        PATHS[2]: b"model geometry stays unselected",
        PATHS[3]: textures(0x87654321),
    }
    config.write_text("; personal settings must stay unchanged\n[game]\ndisc=scene.iso\n")
    original = config.read_bytes()
    base = [executable, "--experimental-scene", "--config", str(config),
            "--model", "/scene.rlg", "--textures", "/scene.rlt", "--particles", "--frames", "90"]

    def run(arguments, expected, message):
        result = subprocess.run(arguments, text=True, capture_output=True, timeout=50)
        output = result.stdout + result.stderr
        assert result.returncode == expected and message in output, output
        assert "VUID-" not in output and "Validation Error" not in output, output
        assert config.read_bytes() == original
        return output

    write_disc(disc, files=payloads, fst_capacity=0x800)
    output = run(base, 0, "Original particle preview rendered:")
    counts = re.search(r"Original particle preview rendered: (\d+) updates, (\d+) submitted quads, (\d+) peak live", output)
    assert counts and int(counts[1]) == 90 and int(counts[2]) > 0 and int(counts[3]) > 0, output
    assert "Original graphics shutdown recovered both game arenas." in output, output
    run(base + ["--nis-primary", "/a.nis", "--nis-secondary", "/b.nis"], 2,
        "--particles cannot be combined with PIP or shadow options")
    del payloads[PATHS[0]]
    write_disc(disc, files=payloads, fst_capacity=0x800)
    run(base, 1, "Particle file is missing")
print("Synthetic particle preview, missing-file, option and unchanged-settings checks passed")
