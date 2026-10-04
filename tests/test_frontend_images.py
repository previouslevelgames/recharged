"""Generated image directories and real NL transaction failures; no retail data."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from frontend_image_fixture import bundle, files, texture


def main():
    executable = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="mscharged-frontend-images-") as folder:
        root = Path(folder)
        (root / "formats.bundle").write_bytes(bundle([(0x100 + fmt, texture(fmt, 2, value=fmt + 17)) for fmt in range(9)]))
        (root / "empty.bundle").write_bytes(bundle([]))
        original = bundle([(0x11, texture()), (0x22, texture(8))])
        (root / "valid.bundle").write_bytes(original)
        corruptions = [("sector", 0, 0), ("count", 4, 4097), ("directory-overflow", 8, 0xffffffff),
            ("directory-header", 8, 0), ("data-overlap", 12, 1), ("data-overflow", 12, 0xffffffff),
            ("block-before-data", 36, 1), ("block-overflow", 36, 0xffffffff), ("zero-length", 40, 0),
            ("size-overflow", 40, 0xffffffff), ("entry-overlap", 48, struct.unpack_from(">I", original, 36)[0])]
        offset = struct.unpack_from(">I", original, 36)[0] * 32
        corruptions += [("levels", offset, 0), ("format", offset + 4, 9),
            ("palette", offset + 20, 1), ("dimensions", offset + 12, 0)]
        for name, at, value in corruptions:
            changed = bytearray(original)
            struct.pack_into(">I", changed, at, value)
            (root / (name + ".bad")).write_bytes(changed)
        changed = bytearray(original)
        changed[offset + 12] = 1
        (root / "missing.bad").write_bytes(changed)
        for mode in ("success", "missing-main", "missing-permanent", "missing-demand", "bad-main", "bad-demand", "empty-main"):
            payloads = files()
            if mode != "success":
                path = "art/fe/" + ("InGameUI.Res" if mode == "missing-permanent" else
                    "InGameUI.Dmn" if mode in ("missing-demand", "bad-demand") else "MainUI.Dmn")
                if mode.startswith("missing-"):
                    del payloads[path]
                else:
                    payloads[path] = b"" if mode == "empty-main" else b"not a valid image bundle"
            write_disc(root / "images.iso", files=payloads)
            result = subprocess.run([executable, str(root / "images.iso"), str(root), mode], timeout=45)
            if result.returncode:
                return result.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
