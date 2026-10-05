"""Original retail boot handler and packets through Vulkan, using generated data."""
from pathlib import Path
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from frontend_boot_loading_fixture import files

executable = str(Path(sys.argv[1]).resolve())
audio_fixture = str(Path(sys.argv[2]).resolve())
with tempfile.TemporaryDirectory(prefix="mscharged-boot-scene-") as directory:
    root = Path(directory)
    config = root / "settings.ini"
    config.write_text("; retained settings\n[game]\ndisc=boot.iso\nlanguage=english\n")
    before = config.read_bytes()
    payloads = files()
    audio = root / "audio-fixture"
    subprocess.run([audio_fixture, "--fixture", str(audio)], check=True, timeout=15)
    payloads.update({"audio/" + path.name: path.read_bytes() for path in audio.iterdir()})
    write_disc(root / "boot.iso", files=payloads)
    base = [executable, "--experimental-scene", "--frontend-boot", "--config", str(config), "--frame-timeout", "60"]

    def run(extra, code, message):
        result = subprocess.run(base + extra, text=True, capture_output=True, timeout=70)
        output = result.stdout + result.stderr
        assert result.returncode == code and message in output, (result.returncode, output)
        assert "VUID-" not in output and "Validation Error" not in output, output
        assert config.read_bytes() == before
        return output

    # Generated slides last0.5s: 1100 fixed60Hz updates cover15.5s strap,
    # 0.5s fade, three slides and the final0.5s wait. The wall-clock bound also
    # permits slower renderers without changing any original simulation timing.
    output = run(["--frames", "1100"], 0, "Retail boot screen rendered: 1100 frames")
    assert "Original graphics shutdown recovered both game arenas." in output, output
    phases = ["Original boot screen phase: " + value for value in ("2", "0", "3", "4")]
    assert all(value in output for value in phases), output
    assert [output.index(value) for value in phases] == sorted(output.index(value) for value in phases)
    assert "Original logo cue admitted to SDL; selected sample " in output, output
    assert "Original boot bank unload completed; startup-manager readiness remains pending." in output, output
    assert "Retail boot final phase: 4; elapsed -1.000000" in output, output
    assert "Boot screen stopped at FEAudio::PlaySound" not in output, output
    # No 3D assets exist in this fixture: the standalone screen must need none.
    assert "Selected model 0x" not in output, output
    run([], 2, "--frame-timeout requires --frames")
    for value in ("0", "601", "invalid"):
        run(["--frames", "30", "--frame-timeout", value], 2, "Invalid --frame-timeout value")
    for extra in (["--frontend-frame", "/Art/fe/boot_loading.fen"], ["--particles"], ["--model", "/absent.rlg"]):
        run(["--frames", "30"] + extra, 2, "--frontend-boot selects its own retail scene")
    del payloads["Art/fe/BootLoadingUI.res"]
    write_disc(root / "boot.iso", files=payloads)
    run(["--frames", "30"], 1, "Frontend image bundle is missing")
print("Retail boot packets, logo audio, phase order, bank unload and teardown passed")
