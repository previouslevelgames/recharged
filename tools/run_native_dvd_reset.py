#!/usr/bin/env python3
"""Real native DVD reset/cache gates using synthetic Wii fixture bytes only."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from disc_fixture import write_disc


def write_cached_dvd_fixture(path):
    write_disc(path, files={
        'payload.bin': bytes((i * 37 + 11) & 255 for i in range(1024)),
        'cached.bin': bytes((i * 37 + 11) & 255 for i in range(262144 + 73)),
        'cold.bin': b'',
    }, partition_size=0x50000)
    # Keep the existing tiny fixture writer's bounds intact. Extend this one explicit
    # synthetic Wii partition and relocate only its named cold file to sector group 1.
    data = bytearray(path.read_bytes())
    base = 0x58000
    partition_size = 0x208000
    data.extend(bytes(base + partition_size - len(data)))
    struct.pack_into('>I', data, 0x502BC, partition_size >> 2)
    fst = base + 0x3000
    entries = struct.unpack_from('>I', data, fst + 8)[0]
    strings = fst + entries * 12
    found = 0
    for i in range(1, entries):
        name = struct.unpack_from('>I', data, fst + i * 12)[0] & 0xFFFFFF
        end = data.index(0, strings + name)
        if data[strings + name:end] == b'cold.bin':
            struct.pack_into('>II', data, fst + i * 12 + 4, 0x200000 >> 2, 128)
            found += 1
    assert found == 1
    data[base + 0x200000:base + 0x200000 + 128] = bytes((i * 37 + 11) & 255 for i in range(128))
    path.write_bytes(data)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="charged-dvd-reset-") as directory:
        path = Path(directory) / "reset.iso"
        write_cached_dvd_fixture(path)
        raise SystemExit(subprocess.run([str(Path(sys.argv[1]).resolve()), str(path)], timeout=20).returncode)
