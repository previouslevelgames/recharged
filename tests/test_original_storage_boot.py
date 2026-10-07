"""Real nod/whole SDK NAND boot fixture; no game save/readiness data."""
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

from disc_fixture import write_disc


def run(executable):
    with tempfile.TemporaryDirectory(prefix="mscharged-storage-boot-") as directory:
        root = pathlib.Path(directory)
        image = root / "title.iso"
        title = 0x0123456789ABCDEF
        group = 0xA1B2
        tmd = bytearray(0x1E4)
        struct.pack_into(">I", tmd, 0, 0x00010001)
        struct.pack_into(">Q", tmd, 0x18C, title)
        struct.pack_into(">H", tmd, 0x198, group)
        write_disc(image, tmd=tmd)
        backing = root / "backing"
        observations = []
        rejected = 0

        def attempt(disc, target, uid, mode, success=True):
            nonlocal rejected
            value = subprocess.run(
                [executable, str(disc), str(target), uid, mode],
                capture_output=True, text=True, timeout=20)
            assert (value.returncode == 0) == success, value.stdout + value.stderr
            if success:
                observation = json.loads(value.stdout.splitlines()[-1])
                assert int(observation["title"], 16) == title
                assert observation["group"] == group and observation["uid"] == 0x1001
                observations.append(observation)
            else:
                rejected += 1

        attempt(image, backing, "0x1001", "fresh")
        attempt(image, backing, "0x1001", "reopen")
        raw = (root / "actual-tmd.bin").read_bytes()
        assert struct.unpack_from(">Q", raw, 0x18C)[0] == title
        assert struct.unpack_from(">H", raw, 0x198)[0] == group
        metadata = backing / "metadata.txt"
        initial = metadata.read_bytes()
        attempt(image, backing, "0x1002", "reopen", False)
        assert metadata.read_bytes() == initial
        attempt(image, root / "reserved", "0x1000", "fresh", False)
        assert not (root / "reserved").exists()
        changed = root / "changed.iso"
        struct.pack_into(">Q", tmd, 0x18C, title ^ 1)
        write_disc(changed, tmd=tmd)
        attempt(changed, backing, "0x1001", "reopen", False)
        assert metadata.read_bytes() == initial
        struct.pack_into(">Q", tmd, 0x18C, title)
        struct.pack_into(">H", tmd, 0x198, group ^ 1)
        write_disc(changed, tmd=tmd)
        attempt(changed, backing, "0x1001", "reopen", False)
        assert metadata.read_bytes() == initial

        # Explicit existing permissions are retained, rather than migrated.
        legacy = root / "legacy"
        shutil.copytree(backing, legacy)
        home = f"/title/{title >> 32:08x}/{title & 0xFFFFFFFF:08x}/data"
        lines = (legacy / "metadata.txt").read_text().splitlines()
        for index, line in enumerate(lines):
            if line.startswith('"' + home + '" '):
                fields = line.split()
                assert fields[3:6] == ["3", "0", "0"]
                fields[4:6] = ["3", "1"]
                lines[index] = " ".join(fields)
        (legacy / "metadata.txt").write_text("\n".join(lines) + "\n")
        before = (legacy / "metadata.txt").read_bytes()
        attempt(image, legacy, "0x1001", "reopen-legacy")
        assert (legacy / "metadata.txt").read_bytes() == before
        print("original_storage_boot:", sum(v["checks"] for v in observations),
              "original checks,", sum(v["completions"] for v in observations),
              "source completions,", rejected, "identity errors; no game saves/readiness")


if __name__ == "__main__":
    run(sys.argv[1])
