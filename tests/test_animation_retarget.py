#!/usr/bin/env python3
"""Independent retarget byte/order/map oracle, optional owned character audit."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from animation_retarget_fixture import chunks, retarget_fixture, retarget_oracle


def execute(executable, *args):
    run = subprocess.run([executable, *map(str, args)], text=True, capture_output=True, timeout=10)
    if run.returncode:
        raise AssertionError(run.stdout + run.stderr)
    return run.stdout.splitlines()


def verify(executable, path):
    raw = path.read_bytes()
    expected = []
    for identity, maps in retarget_oracle(raw):
        expected.append(f"LIST {identity} {len(maps)}")
        for signature, metadata, nodes in maps:
            expected.append(" ".join(map(str, ("MAP", signature, metadata, len(nodes), *nodes))))
    actual = execute(executable, "--inspect", path)
    assert actual[:-1] == expected, (path, actual, expected)
    return {"file": path.name, "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw), "lists": retarget_oracle(raw)}


def main():
    executable = str(Path(sys.argv[1]).resolve())
    if len(sys.argv) == 3:
        root = Path(sys.argv[2])
        profiles = ("mario", "bowser", "daisy", "donkeykong", "luigi", "peach", "waluigi", "wario", "yoshi",
                    "bowserjr", "diddykong", "petey", "birdo", "hammerbro", "koopa", "toad", "boo", "drybones", "montymole", "shyguy")
        result = []
        for index, name in enumerate(profiles):
            folder = root / "Art/animation"
            path = folder / name / "animretarget" / (name + ".bin")
            row = verify(executable, path)
            hierarchy = (folder / (name + ".shier")).read_bytes()
            outer = next(chunks(hierarchy))
            children = {kind: hierarchy[start:end] for kind, start, end, _ in chunks(hierarchy, outer[1], outer[2])}
            nodes = struct.unpack_from(">I", children[0x18001], 8)[0]
            mirrors = struct.unpack(">" + "i" * nodes, children[0x18008])
            animations = (folder / (name + "fe.sanim")).read_bytes()
            tracks = []
            maps = retarget_oracle(path.read_bytes())[-1][1]
            expected = []
            for track, (_, start, end, _) in enumerate(chunks(animations)):
                header = next(chunks(animations, start, end))[1]
                count = struct.unpack_from(">I", animations, header + 12)[0]
                signature = struct.unpack_from(">I", animations, header + 84)[0]
                selected = next((m for m in maps if m[0] == signature), None)
                mapping = selected[2] if selected else list(range(nodes))
                assert len(mapping) == nodes and all(n == -1 or 0 <= n < count for n in mapping)
                expected.append(f"TRACK {track} {signature} {count} {int(selected is not None)}")
                expected.extend(f"NODE {n} {mapping[n]} {mapping[mirrors[n]]}" for n in range(nodes))
                tracks.append({"signature": signature, "source_nodes": count, "retargeted": selected is not None})
            expected.insert(0, f"CHAR {index} {nodes} {len(tracks)}")
            actual = execute(executable, "--character", root, index)
            assert actual[:-1] == expected, (name, actual[:5], expected[:5])
            row.update(name=name, target_nodes=nodes, tracks=tracks)
            result.append(row)
        print(json.dumps(result, indent=2))
        return
    with tempfile.TemporaryDirectory(prefix="mscharged-retarget-") as directory:
        path = Path(directory) / "retarget.bin"
        for seed in range(36):
            lists = None if seed % 2 else [[(0x100 + seed, 1, [])], [], [(0x100 + seed, 0, [32767, -1, 0])]]
            path.write_bytes(retarget_fixture(seed, lists))
            verify(executable, path)
        raw = retarget_fixture(3)
        outer = next(chunks(raw))
        children = list(chunks(raw, outer[1], outer[2]))
        nested = list(chunks(raw, children[1][1], children[1][2]))
        mutations = [(0, 0x80000001), (4, 0xffffffff), (children[0][1] + 8, 257),
                     (children[0][1] + 8, 0xffffffff), (nested[0][1] + 8, 4097),
                     (nested[0][1] + 8, 2), (nested[0][1] + 8, 4), (nested[1][3], 0x17107),
                     (nested[0][3], 0x17108), (children[1][3], 0x80017105)]
        for offset, value in mutations:
            bad = bytearray(raw)
            struct.pack_into(">I", bad, offset, value)
            path.write_bytes(bad)
            execute(executable, "--reject", path)
        bad = bytearray(raw)
        struct.pack_into(">h", bad, nested[1][1], -2)
        path.write_bytes(bad)
        execute(executable, "--reject", path)
        for length in range(len(raw)):
            path.write_bytes(raw[:length])
            execute(executable, "--reject", path)
        path.write_bytes(raw + bytes(4))
        execute(executable, "--reject", path)
    print("36 independent retarget inventories, malformed records and every single-list truncation passed")


if __name__ == "__main__":
    main()
