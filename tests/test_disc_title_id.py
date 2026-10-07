"""Native ES title metadata; no retail bytes or inferred disc identity."""
import pathlib
import struct
import subprocess
import sys
import tempfile

from disc_fixture import write_disc


def run(executable):
    with tempfile.TemporaryDirectory(prefix="mscharged-title-") as directory:
        image = pathlib.Path(directory) / "title.iso"
        tmd = bytearray(0x1E4)
        struct.pack_into(">I", tmd, 0, 0x00010001)
        # Deliberately differs from R4QE disc ID: use actual TMD bytes, not a
        # name-derived identity. Full 64-bit value proves BE/header transport.
        struct.pack_into(">Q", tmd, 0x18C, 0x0123456789ABCDEF)
        checks = 0

        def check(metadata, expected=None, game_id=b"R4QE01", partition=True):
            nonlocal checks
            write_disc(image, tmd=metadata, game_id=game_id, partition=partition)
            result = subprocess.run([executable, str(image)], capture_output=True, text=True, timeout=15)
            checks += 1
            if expected is None:
                assert result.returncode != 0, (metadata, result.stdout)
            else:
                assert result.returncode == 0 and result.stdout.strip() == expected, result.stderr

        check(tmd, "0123456789abcdef")
        check(None)
        check(tmd[:0x193])
        bad = bytearray(tmd)
        struct.pack_into(">I", bad, 0, 0x00010000)
        check(bad)
        bad = bytearray(tmd)
        struct.pack_into(">Q", bad, 0x18C, 0)
        check(bad)
        check(tmd, game_id=b"OTHER0")
        check(tmd, partition=False)
        print(f"disc_title_id: {checks} real nod metadata cases")


if __name__ == "__main__":
    run(sys.argv[1])
