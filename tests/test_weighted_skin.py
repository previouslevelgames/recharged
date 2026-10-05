#!/usr/bin/env python3
"""Independent weighted RLG byte/bit oracle; optional private owned assets."""
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile


def chunks(data, begin, end):
    """Parse bytes without a port reader, respecting file-relative alignment."""
    offset = begin
    while offset < end:
        raw, size = struct.unpack_from(">II", data, offset)
        alignment = 2 ** ((raw >> 24) & 127)
        payload = (offset + 8 + alignment - 1) // alignment * alignment
        stop = offset + 8 + size
        assert payload <= stop <= end
        yield raw & 0x80ffffff, payload, stop
        offset = (stop + 3) // 4 * 4
    assert offset == end


def decoded(data):
    def integers(begin, end, width=4):
        assert (end - begin) % width == 0
        return list(struct.unpack_from(">" + ("I" if width == 4 else "H") * ((end-begin)//width), data, begin))
    def at(begin):
        return struct.unpack_from(">I", data, begin)[0]
    def fields(begin, end):
        rows = list(chunks(data, begin, end))
        assert len({kind for kind, _, _ in rows}) == len(rows)
        return {kind: (p, stop) for kind, p, stop in rows}

    root = list(chunks(data, 0, len(data)))
    assert len(root) == 1
    kind, begin, end = root[0]
    if kind == 0x8001b100:
        groups = list(chunks(data, begin, end))
        assert len(groups) == 1
        kind, begin, end = groups[0]
    assert kind == 0x8001b000
    f = fields(begin, end)
    records = {kind: integers(*f[kind]) for kind in (0x1b016, 0x1b002, 0x1b003)}
    model = {"hash": records[0x1b003][0], "binds": [], "packets": []}
    skin = list(chunks(data, *f[0x8001b008]))
    maps = []
    for kind, begin, end in skin:
        if kind == 0x1b00a:
            values = integers(begin, end)
            assert len(values) % 17 == 0
            model["binds"] = [values[i:i+17] for i in range(0, len(values), 17)]
        elif kind == 0x1b00b:
            maps.append(integers(begin, end))
        elif kind == 0x1b00c:
            assert integers(begin, end) == [0, 16, records[0x1b003][1]]
    packet_begin, packet_end = f[0x1b004]
    assert (packet_end-packet_begin)//48 == len(maps) == records[0x1b003][1]
    for i, packet_offset in enumerate(range(packet_begin, packet_end, 48)):
        words = integers(packet_offset, packet_offset+48)
        unique, primitive, count = struct.unpack_from(">HBB", data, packet_offset+8)
        assert count == 7 and words[4] == 0x22cadb20
        material = records[0x1b016][words[8]//4:words[8]//4+18]
        textures = [value for t in range(3) for value in
                    (material[2*t], data[f[0x1b016][0]+words[8]+t*8+6])]
        matrix = records[0x1b002][words[6]*16:words[6]*16+16]
        index_begin = f[0x1b007][0]+words[0]
        indices = integers(index_begin, index_begin+words[1]*2, 2)
        streams, slots, offsets = [], [], []
        for stream in range(7):
            base = f[0x1b005][0]+words[3]+stream*8
            start, slot, stride, kind, unused = struct.unpack_from(">IBBBB", data, base)
            assert (stride,kind,unused) == [(12,1,0),(12,2,0),(4,4,0),(4,4,0),(4,4,0),(4,7,0),(16,5,0)][stream]
            offsets.append(f[0x1b006][0]+start)
            channels = []
            for vertex in range(unique):
                start = offsets[-1]+vertex*stride
                if stream == 5:
                    channels.append(list(data[start:start+4]))
                else:
                    channels.append(integers(start, start+stride, 2 if 2 <= stream <= 4 else 4))
            streams.append(channels)
            slots.append(slot)
        vertices = [sum((streams[stream][vertex] for stream in range(7)), []) for vertex in range(unique)]
        model["packets"].append({"header": [words[4],primitive,words[7]]+slots+matrix+textures+material[8:18],
                                 "bones": maps[i], "indices": indices, "vertices": vertices,
                                 "bone_offset": offsets[5], "weight_offset": offsets[6], "unique": unique})
    return model


def fingerprint(model, prepared=False):
    values = [model["hash"]]
    for bind in model["binds"]:
        values.extend(bind)
    for packet in model["packets"]:
        values.extend(packet["header"] + packet["bones"] + packet["indices"])
        for original in packet["vertices"]:
            vertex = list(original)
            if prepared:
                weights = [struct.unpack(">f", struct.pack(">I", n))[0] for n in vertex[16:20]]
                largest = max(range(4), key=lambda lane: weights[lane])
                vertex[12],vertex[12+largest] = vertex[12+largest],vertex[12]
                vertex[16],vertex[16+largest] = vertex[16+largest],vertex[16]
            values.extend(vertex)
    result = 0xcbf29ce484222325
    for value in values:
        for byte in struct.pack(">I", value):
            result = ((result ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return result


def verify(executable, model_path, hierarchy_path):
    raw = model_path.read_bytes()
    output = subprocess.check_output([executable, str(model_path), str(hierarchy_path)], text=True, timeout=30)
    expected = decoded(raw)
    actual = {k: int(v) for item in output.split() if "=" in item for k,v in [item.split("=",1)]}
    assert actual["raw_fingerprint"] == fingerprint(expected), "Raw byte/bit fingerprint differs"
    assert actual["prepared_fingerprint"] == fingerprint(expected, True), "Original largest-first bit fingerprint differs"
    assert model_path.read_bytes() == raw, "Reader modified game-data bytes"
    weights = [struct.unpack(">f", struct.pack(">I", n))[0]
               for p in expected["packets"] for v in p["vertices"] for n in v[16:20]]
    return {"sha256": hashlib.sha256(raw).hexdigest(), "packets": len(expected["packets"]),
            "vertices": sum(p["unique"] for p in expected["packets"]),
            "raw_fingerprint": actual["raw_fingerprint"], "prepared_fingerprint": actual["prepared_fingerprint"],
            "positive_weights": sum(w > 0 for w in weights), "nodes": actual["nodes"], "rigid": actual["rigid"]}


def main():
    executable = str(Path(sys.argv[1]).resolve())
    if len(sys.argv) == 4:
        print(json.dumps(verify(executable, Path(sys.argv[2]), Path(sys.argv[3])), indent=2))
        return
    assert len(sys.argv) == 2
    subprocess.run([executable], check=True, timeout=30)
    with tempfile.TemporaryDirectory(prefix="charged-weighted-") as directory:
        model_path, hierarchy_path = Path(directory)/"skin.rlg", Path(directory)/"skin.shier"
        subprocess.run([executable, "--fixture", str(model_path), str(hierarchy_path)], check=True, timeout=10)
        baseline = model_path.read_bytes()
        result = [verify(executable, model_path, hierarchy_path)]
        for seed in range(40):
            rng, modified = random.Random(seed), bytearray(baseline)
            for packet in decoded(baseline)["packets"]:
                for vertex in range(packet["unique"]):
                    active = 1 + (seed+vertex) % 4
                    bones = rng.sample(range(4), 4)
                    # Deliberately nonunit sums, ties, all four distinct bones,
                    # signed zero and single influence moved out of lane0.
                    weights = [rng.choice([0.125, 0.25, 0.5, 0.75]) for _ in range(active)] + [-0.0] * (4-active)
                    if active == 1:
                        lane = seed % 4
                        weights[0],weights[lane] = weights[lane],weights[0]
                    for lane in range(4):
                        modified[packet["bone_offset"]+vertex*4+lane] = bones[lane]
                        struct.pack_into(">f", modified, packet["weight_offset"]+vertex*16+lane*4, weights[lane])
            model_path.write_bytes(modified)
            result.append(verify(executable, model_path, hierarchy_path))
    print(json.dumps({"independent_byte_oracles": len(result), "results": result}, indent=2))


if __name__ == "__main__":
    main()
