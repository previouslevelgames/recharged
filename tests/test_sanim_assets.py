#!/usr/bin/env python3
"""Independent big-endian animation/key/sampling oracle; no game data required."""
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile

TIMES = (0, .125, .25, .5, .75, .875, 1)


def f32(x):
    return struct.unpack(">f", struct.pack(">f", x))[0]


def bits(x):
    return struct.unpack(">I", struct.pack(">f", x))[0]


def words(values):
    return b"".join(struct.pack(">I", x & 0xffffffff) for x in values)


def halves(values):
    return b"".join(struct.pack(">H", x & 0xffff) for x in values)


def digest(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def fixture(seed):
    rng = random.Random(seed)
    data = bytearray()

    def start(kind):
        exponent = rng.randrange(6)
        offset = len(data)
        data.extend(words([kind | (exponent << 24), 0]))
        data.extend(b"\xcc" * (-len(data) % (1 << exponent)))
        return offset

    def finish(offset):
        struct.pack_into(">I", data, offset + 4, len(data) - offset - 8)
        data.extend(b"\x5a" * (-len(data) % 4))

    def chunk(kind, payload):
        offset = start(kind)
        data.extend(payload)
        finish(offset)

    for track in range(1 + seed % 3):
        root = start(0x80017000)
        frames, nodes, roots = rng.randrange(1, 10), 5 if seed else 0, seed % 4
        morph_counts = [] if seed % 4 == 0 else [1, 3] if seed % 4 == 1 else [3, 3]
        header = [rng.getrandbits(32) for _ in range(22)]
        header[1:5] = [seed * 4 + track, frames, nodes, len(morph_counts)]
        header[13] = roots
        chunk(0x17001, words(header))
        name = f"generated_{seed}_{track}".encode() + b"\0"
        chunk(0x17002, name + bytes(-len(name) % 4))
        counts = [1, 3, 0, 5, 0][:nodes]
        chunk(0x17110, words(counts))
        chunk(0x17113, words(rng.getrandbits(32) for _ in range(nodes)))
        for kind in (0x17004, 0x17005, 0x17006, 0x17111, 0x17114):
            chunk(kind, words(rng.getrandbits(32) for _ in range(nodes)))
        chunk(0x17007, halves(rng.randrange(65536) for _ in range(roots)))
        chunk(0x17008, words(bits(rng.uniform(-16, 16)) for _ in range(roots * 3)))
        properties = [15, 16, 46, 0x40000000, 0][:nodes]
        for node, prop in enumerate(properties):
            offset = start(0x80017100)
            if node != 4:
                width = 2 if prop & 1 else 8 if prop & 16 else 6 if prop & 32 else 4
                chunk(0x17101, bytes(rng.randrange(256) for _ in range((1 if prop & 2 else frames) * width)))
                chunk(0x17102, words(bits(rng.uniform(-50, 50)) for _ in range((1 if prop & 4 else frames) * 3)))
                chunk(0x17103, halves(rng.randrange(65536) for _ in range((1 if prop & 8 else frames) * 3)))
                if counts[node]:
                    chunk(0x17112, bytes(rng.randrange(256) for _ in range(counts[node])))
                chunk(0x17115, bytes(rng.randrange(256) for _ in range(seed % 7)))
            finish(offset)
        chunk(0x17009, words(morph_counts))
        chunk(0x1700a, words(rng.getrandbits(32) for _ in morph_counts))
        chunk(0x1700b, bytes(rng.randrange(256) for _ in range(sum(morph_counts))))
        chunk(0x17003, words(properties))
        finish(root)
    return bytes(data)


def oracle(data):
    def chunks(start, end):
        while start < end:
            raw, length = struct.unpack_from(">II", data, start)
            align = 1 << ((raw >> 24) & 127)
            begin = (start + 8 + align - 1) // align * align
            stop = start + 8 + length
            assert begin <= stop <= end
            yield raw & 0x80ffffff, begin, stop
            start = (stop + 3) // 4 * 4

    def integers(payload, width=4):
        return list(struct.unpack(">" + ("I" if width == 4 else "H") * (len(payload) // width), payload))

    def scalar_samples(keys):
        result = []
        for t in TIMES:
            if len(keys) == 1 or t == 1:
                value = f32(keys[-1] / 255)
            else:
                index = f32(t * (len(keys) - 1))
                at = int(index)
                fraction = f32(index - at)
                value = f32(f32(f32(1 - fraction) * f32(keys[at] / 255)) + f32(fraction * f32(keys[at+1] / 255)))
            result.append(bits(value))
        return result

    animations = []
    for kind, begin, stop in chunks(0, len(data)):
        assert kind == 0x80017000
        children = list(chunks(begin, stop))
        payloads = {kind: data[a:b] for kind, a, b in children if kind != 0x80017100}
        header = integers(payloads[0x17001])
        props = integers(payloads[0x17003])
        aux_metadata = integers(payloads[0x17113])
        result = {"name": payloads[0x17002].split(b"\0", 1)[0].decode("ascii"),
                  "hash": header[1], "frames": header[2], "signature": header[21], "nodes": []}
        node_index = 0
        for kind, a, b in children:
            if kind != 0x80017100:
                continue
            node = {kind: data[c:d] for kind, c, d in chunks(a, b)}
            prop = props[node_index]
            rotation, translation, scale = node.get(0x17101, b""), node.get(0x17102, b""), node.get(0x17103, b"")
            width = 2 if prop & 1 else 8 if prop & 16 else 6 if prop & 32 else 4
            decoded_rotation = bytearray()
            for offset in range(0, len(rotation), width):
                raw = rotation[offset:offset+width]
                if prop & 1:
                    decoded_rotation.extend(raw)
                    continue
                if width == 8:
                    values, divisor = struct.unpack(">hhhh", raw), 32768
                elif width == 6:
                    packed = int.from_bytes(raw, "big")
                    values = [((packed >> shift) & 4095) for shift in (36, 24, 12, 0)]
                    values = [v - 4096 if v >= 2048 else v for v in values]
                    divisor = 2048
                else:
                    values, divisor = struct.unpack("bbbb", raw), 128
                decoded_rotation.extend(words(bits(v / divisor) for v in values))
            decoded_scale = words(bits(v / 2048) for v in integers(scale, 2))
            weights = node.get(0x17112, b"")
            result["nodes"].append({"properties": prop, "aux_metadata": aux_metadata[node_index], "aux_present": 0x17115 in node,
                                    "counts": [len(rotation)//width, len(scale)//6, len(translation)//12, len(weights)],
                                    "hashes": [digest(decoded_rotation), digest(decoded_scale), digest(translation),
                                               digest(weights), digest(node.get(0x17115, b""))],
                                    "weights": scalar_samples(weights) if weights else [bits(1)]*len(TIMES)})
            node_index += 1
        assert node_index == header[3]
        root_angles = integers(payloads[0x17007], 2)
        floats = struct.unpack(">" + "f"*(len(payloads[0x17008])//4), payloads[0x17008])
        roots = [floats[i:i+3] for i in range(0, len(floats), 3)]
        samples = []
        for time in TIMES:
            if not roots:
                angle, translation = 0, (0, 0, 0)
            elif len(roots) == 1 or time == 1:
                angle, translation = root_angles[-1], roots[-1]
            else:
                index = f32(time * (len(roots) - 1))
                at, fraction = int(index), f32(index-int(index))
                delta = (root_angles[at+1] - root_angles[at]) & 65535
                if delta >= 32768:
                    delta -= 65536
                angle = (root_angles[at] + int(f32(fraction * delta))) & 65535
                translation = [f32(f32(f32(1-fraction)*a) + f32(fraction*b)) for a, b in zip(roots[at], roots[at+1])]
            samples.append([angle] + [bits(v) for v in translation])
        result["root_samples"] = samples
        if roots:
            delta = [f32(b-a) for a,b in zip(roots[0], roots[-1])]
            squared = [f32(v*v) for v in delta]
            result["speed"] = f32(f32(math.sqrt(f32(f32(squared[0]+squared[1])+squared[2]))) / f32(header[2]/30))
        else:
            result["speed"] = 0
        counts = integers(payloads[0x17009])
        result["morph_ids"] = integers(payloads[0x1700a])
        result["morph_counts"] = counts
        keys = payloads[0x1700b]
        result["morph_hash"] = digest(keys)
        result["morph_samples"] = None if len(set(counts)) > 1 else [scalar_samples(keys[i*count:(i+1)*count]) for i,count in enumerate(counts)]
        animations.append(result)
    return animations


def verify(executable, path):
    before = path.read_bytes()
    expected = oracle(before)
    actual = json.loads(subprocess.check_output([executable, "--inspect", str(path)], text=True, timeout=10))
    assert len(expected) == len(actual)
    for a, e in zip(actual, expected):
        speed, original_speed = a.pop("speed"), e.pop("speed")
        assert math.isclose(speed, original_speed, rel_tol=1e-6, abs_tol=1e-6), (path, speed, original_speed)
        assert a == e, f"Native keys/original samples differ for {path}, {e['name']}"
    assert before == path.read_bytes()
    return {"path": str(path), "bytes": len(before), "sha256": hashlib.sha256(before).hexdigest(),
            "animations": len(expected), "nodes": sum(len(a["nodes"]) for a in expected),
            "rotation_keys": sum(n["counts"][0] for a in expected for n in a["nodes"])}


def main():
    executable = str(Path(sys.argv[1]).resolve())
    if len(sys.argv) == 3:
        paths = sorted(Path(sys.argv[2]).rglob("*.sanim"))
        assert paths, "No owned SAnim files found"
        results = [verify(executable, path) for path in paths]
        print(json.dumps({"files": len(results), "animations": sum(r["animations"] for r in results), "results": results}, indent=2))
    else:
        with tempfile.TemporaryDirectory(prefix="charged-sanim-") as directory:
            for seed in range(36):
                path = Path(directory) / f"generated-{seed}.sanim"
                path.write_bytes(fixture(seed))
                verify(executable, path)
        print("36 independent SAnim inventory/key/sampling oracles passed")


if __name__ == "__main__":
    main()
