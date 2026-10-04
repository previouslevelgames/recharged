"""Real NL async loading, cancellation and transactional failure of FE visuals."""
from pathlib import Path
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from frontend_visual_fixture import files

executable = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="mscharged-frontend-visuals-") as folder:
    root = Path(folder)
    for mode in ("success", "missing-loc", "missing-font", "bad-loc", "bad-text", "bad-heading", "empty"):
        payloads = files()
        if mode in ("missing-loc", "bad-loc"):
            for language in ("english", "nafrench", "naspanish"):
                path = "Art/fe/" + language + ".loc"
                if mode == "missing-loc": del payloads[path]
                else: payloads[path] = b"malformed"
        elif mode != "success":
            path = "Art/fe/fonts/" + ("eurfonttext18.res" if mode in ("bad-text", "empty") else "eurfontheading36.res")
            if mode == "missing-font": del payloads[path]
            else: payloads[path] = b"" if mode == "empty" else b"malformed"
        write_disc(root / "visuals.iso", files=payloads)
        subprocess.run([executable, str(root / "visuals.iso"), str(root), mode], check=True, timeout=30)
