"""Real synthetic Wii/TMD backing for native shutdown request/boot metadata."""
import pathlib
import struct
import subprocess
import sys
import tempfile

from disc_fixture import write_disc


def run(executable):
    with tempfile.TemporaryDirectory(prefix="mscharged-os-shutdown-requests-") as directory:
        root = pathlib.Path(directory)
        image = root / "title.iso"
        tmd = bytearray(0x1e4)
        struct.pack_into(">I", tmd, 0, 0x00010001)
        struct.pack_into(">Q", tmd, 0x18c, 0x0001000052345145)
        struct.pack_into(">H", tmd, 0x198, 0xa1b2)
        write_disc(image, tmd=tmd)
        value = subprocess.run([executable, str(image), str(root/"backing")],
                               capture_output=True, text=True, timeout=20)
        print(value.stdout, end="")
        print(value.stderr, end="", file=sys.stderr)
        if value.returncode:
            raise SystemExit(value.returncode)


if __name__ == "__main__":
    run(sys.argv[1])
