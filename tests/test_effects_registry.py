from pathlib import Path
import subprocess
import sys
import tempfile
from effects_registry_fixture import chunk, entry, files, resident, textures


def main():
    with tempfile.TemporaryDirectory(prefix="mscharged-effects-registry-") as folder:
        root = Path(folder)
        for name, data in files().items():
            (root / name).write_bytes(data)
        cases = {"persistent-fountain": resident(entry(persistent=1)),
                 "persistent-particle": resident(entry(persistent=2)),
                 "duplicate": resident(entry(), entry(20)),
                 "user": resident(entry(user=True)),
                 "shadowed": resident(entry(), entry(20, user=True)),
                 "restored": resident(entry(user=True), entry(20)),
                 "bad-enum": resident(entry(binding=0xffffffff)),
                 "unbound": resident(entry(texture=0xffffffff, model=0x12341234)),
                 "empty": resident()}
        # Signed metadata transport only; no particle update/switch is run.
        for axis in (*range(7), -1, -2147483648, 2147483647):
            cases[f"forward-axis-{axis}"] = resident(entry(forward_axis=axis))
        for name, data in cases.items():
            (root / (name + ".bun")).write_bytes(data)
        (root / "same.rlt").write_bytes(textures())
        (root / "conflict.rlt").write_bytes(textures(marker=65))
        result = subprocess.run([str(Path(sys.argv[1]).resolve()), str(root)], timeout=30)
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
