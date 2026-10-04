#!/usr/bin/env python3
"""Independent SHierarchy format oracle; optional local owned-asset audit."""
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile


def words(values):
    return b"".join(struct.pack(">I", n & 0xffffffff) for n in values)


def fixture(seed):
    rng = random.Random(seed)
    parents = [-1]

    def children(parent, depth):
        for _ in range(rng.randrange(1, 5) if depth == 0 else rng.randrange(4)):
            index = len(parents)
            parents.append(parent)
            if depth < 4:
                children(index, depth + 1)

    if seed:
        children(0, 0)
    count = len(parents)
    ids = [rng.getrandbits(32) for _ in parents]
    groups = [[i for i, p in enumerate(parents) if p == node] for node in range(count)]
    name = f"independent_{seed}".encode() + b"\0"
    name += bytes(-len(name) % 4)
    header = [rng.getrandbits(32) for _ in range(13)]
    header[1], header[2], header[9], header[10] = seed + 7000, count, -1, -1
    translations = [bits for i in range(count) for bits in
                    (0x80000000 if i == 0 else struct.unpack(">I", struct.pack(">f", rng.uniform(-500, 500)))[0],
                     1 if i == 0 else struct.unpack(">I", struct.pack(">f", rng.uniform(-500, 500)))[0],
                     struct.unpack(">I", struct.pack(">f", rng.uniform(-500, 500)))[0])]
    channels = [
        (0x18001, words(header)), (0x18002, name), (0x18003, words(ids)),
        (0x18009, words(parents)), (0x18004, words(map(len, groups))),
        (0x18005, words(rng.getrandbits(32) for _ in parents)),
        (0x18006, words(rng.getrandbits(32) for _ in parents)),
        (0x18007, words(n for group in groups for n in group)),
        (0x18008, words(rng.randrange(count) for _ in parents)),
        (0x18010, words(translations)),
        (0x18011, bytes(rng.randrange(256) for _ in parents)),
    ]
    outer_exp = seed % 6
    data = bytearray(words([0x80018000 | (outer_exp << 24), 0]))
    data.extend(b"\xcc" * (-len(data) % (1 << outer_exp)))
    for kind, payload in channels:
        exponent = rng.randrange(6)
        start = len(data)
        data.extend(words([kind | (exponent << 24), 0]))
        data.extend(b"\xa5" * (-len(data) % (1 << exponent)))
        data.extend(payload)
        struct.pack_into(">I", data, start + 4, len(data) - start - 8)
        data.extend(b"\x5a" * (-len(data) % 4))
    struct.pack_into(">I", data, 4, len(data) - 8)
    return bytes(data)


def oracle(data):
    # Independent Python byte slicing and struct decoding, without any port
    # reader/helper or generated native structs. Only valid inputs reach here.
    def chunk(offset):
        raw, size = struct.unpack_from(">II", data, offset)
        alignment = 2 ** ((raw >> 24) & 127)
        begin = ((offset + 8 + alignment - 1) // alignment) * alignment
        end = offset + 8 + size
        return raw & 0x80ffffff, data[begin:end], begin, (end + 3) // 4 * 4

    kind, _, at, end = chunk(0)
    assert kind == 0x80018000 and end == len(data)
    payloads = {}
    while at < end:
        kind, payload, _, at = chunk(at)
        assert kind not in payloads
        payloads[kind] = payload

    def integers(kind, signed=False):
        payload = payloads[kind]
        assert len(payload) % 4 == 0
        return list(struct.unpack(">" + ("i" if signed else "I") * (len(payload) // 4), payload))

    h = integers(0x18001)
    count = h[2]
    parents = integers(0x18009, True)
    depths = []
    for i, parent in enumerate(parents):
        assert (i == 0 and parent == -1) or 0 <= parent < i
        depths.append(0 if i == 0 else depths[parent] + 1)
    ids, mirrors = integers(0x18003), integers(0x18008)
    counts, flat = integers(0x18004), integers(0x18007)
    translations = integers(0x18010)
    preserve = payloads[0x18011]
    result = {"name": payloads[0x18002].split(b"\0", 1)[0].decode("ascii"),
              "hash": h[1], "pelvis": struct.unpack(">i", words([h[9]]))[0],
              "spine": struct.unpack(">i", words([h[10]]))[0],
              "depth": max(depths), "nodes": []}
    cursor = 0
    for i in range(count):
        result["nodes"].append({"id": ids[i], "parent": parents[i], "mirror": mirrors[i],
                                "push_pop": depths[i+1] - depths[i] if i+1 < count else 0,
                                "preserve": preserve[i], "translation_bits": translations[3*i:3*i+3],
                                "children": flat[cursor:cursor+counts[i]]})
        cursor += counts[i]
    assert cursor == count - 1
    return result


def verify(executable, path):
    before = path.read_bytes()
    actual = json.loads(subprocess.check_output([executable, "--inspect", str(path)], text=True, timeout=10))
    expected = oracle(before)
    assert actual == expected, f"Original native fields/traversal differ for {path}"
    assert before == path.read_bytes(), f"Asset bytes changed for {path}"
    return {"path": str(path), "bytes": len(before), "nodes": len(expected["nodes"]),
            "depth": expected["depth"], "sha256": hashlib.sha256(before).hexdigest()}


def main():
    executable = str(Path(sys.argv[1]).resolve())
    if len(sys.argv) == 3:
        paths = sorted(Path(sys.argv[2]).rglob("*.shier"))
        assert paths, "Owned hierarchy directory contains no SHierarchy files"
        results = [verify(executable, path) for path in paths]
        print(json.dumps({"files": len(results), "nodes": sum(r["nodes"] for r in results), "results": results}, indent=2))
    else:
        with tempfile.TemporaryDirectory(prefix="charged-hierarchy-") as directory:
            for seed in range(36):
                path = Path(directory) / f"fixture-{seed}.shier"
                path.write_bytes(fixture(seed))
                verify(executable, path)
        print("36 independent hierarchy format/traversal oracles passed")


if __name__ == "__main__":
    main()
