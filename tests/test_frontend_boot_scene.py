"""Original retail boot handler and packets through Vulkan, using generated data."""
from pathlib import Path
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from frontend_boot_loading_fixture import files

executable = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="mscharged-boot-scene-") as directory:
    root = Path(directory)
    config = root / "settings.ini"
    config.write_text("; retained settings\n[game]\ndisc=boot.iso\nlanguage=english\n")
    before = config.read_bytes()
    payloads = files()
    write_disc(root / "boot.iso", files=payloads)
    base = [executable, "--experimental-scene", "--frontend-boot", "--config", str(config)]

    def run(extra, code, message):
        result = subprocess.run(base + extra, text=True, capture_output=True, timeout=70)
        output = result.stdout + result.stderr
        assert result.returncode == code and message in output, (result.returncode, output)
        assert "VUID-" not in output and "Validation Error" not in output, output
        assert config.read_bytes() == before
        return output

    output = run(["--frames", "1100"], 0, "Retail boot screen rendered: 1100 frames")
    assert "Original graphics shutdown recovered both game arenas." in output, output
    phases = ["Original boot screen phase: " + value for value in ("2", "0", "3")]
    assert all(value in output for value in phases), output
    assert [output.index(value) for value in phases] == sorted(output.index(value) for value in phases)
    assert "Boot screen stopped at FEAudio::PlaySound(0x17, 0xde83984e)." in output, output
    # No 3D assets exist in this fixture: the standalone screen must need none.
    assert "Selected model 0x" not in output, output
    for extra in (["--frontend-frame", "/Art/fe/boot_loading.fen"], ["--particles"], ["--model", "/absent.rlg"]):
        run(extra, 2, "--frontend-boot selects its own retail scene")
    del payloads["Art/fe/BootLoadingUI.res"]
    write_disc(root / "boot.iso", files=payloads)
    run(["--frames", "30"], 1, "Frontend image bundle is missing")
print("Retail boot packets, phase order, audio boundary and teardown passed")
