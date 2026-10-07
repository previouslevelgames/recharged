"""Whole original flash task/allocator/NAND gate; no game save readiness."""

import json
import pathlib
import struct
import subprocess
import sys
import tempfile

from disc_fixture import write_disc


def run(executable, module):
    with tempfile.TemporaryDirectory(prefix="mscharged-original-flash-") as directory:
        root = pathlib.Path(directory)
        image = root / "owned-fixture.iso"
        tmd = bytearray(0x1E4)
        struct.pack_into(">I", tmd, 0, 0x00010001)
        struct.pack_into(">Q", tmd, 0x18C, 0x0001000052345145)
        struct.pack_into(">H", tmd, 0x198, 0x3031)
        write_disc(image, tmd=tmd)
        result = subprocess.run(
            [executable, module, str(image), str(root / "backing")],
            capture_output=True, text=True, timeout=20)
        assert result.returncode == 0, result.stdout + result.stderr
        observation = json.loads(result.stdout.splitlines()[-1])
        assert observation["source_user_callbacks"] == 18
        assert observation["irq_completions"] == 27
        assert observation["original_task_runs"] == 50
        assert observation["allocated_read_bytes"] == 128
        assert observation["raw_read_result"] == 97
        assert observation["title"] == "0001000052345145"
        assert observation["group"] == 0x3031
        print("original_flash:", observation["checks"], "checks,",
              observation["source_user_callbacks"], "original task callbacks;",
              "whole startup/task admission and saves remain pending")


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
