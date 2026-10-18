"""Readers for the robot goalie map: GameCube (.glg) and Wii (.rlg) character models,
their skeletons (.shier) and GX textures (.glt, .rlt).

Layouts follow the two decompilations: the GameCube game's glxLoadModel.cpp and
gluSkinMesh.cpp (Super Mario Strikers) and Charged's glLoadModel.cpp, glModel.h
and nlChunk.h. Only what the map needs is read: bind-pose positions, texture
coordinates and the bone of each vertex.
"""

import struct


def _gc_chunks(data, offset, end):
    """GameCube nlChunk: id with alignment bits 24-30, next chunk right after the data."""
    out = []
    while offset + 8 <= end:
        raw, size = struct.unpack_from(">II", data, offset)
        bits = raw & 0x7F000000
        start = offset + 8
        if bits:
            align = 1 << (bits >> 24)
            start = (start + align - 1) & ~(align - 1)
        out.append((raw & ~0x7F000000 & 0xFFFFFFFF, start, size))
        offset = offset + 8 + size
    return out


def _wii_chunks(data, offset, end):
    """Charged nlChunk: id & 0x80FFFFFF, data alignment in bits 24-27, next chunk 4-aligned."""
    out = []
    while offset + 8 <= end:
        raw, size = struct.unpack_from(">II", data, offset)
        start = offset + 8
        bits = raw & 0x0F000000
        aligned = (start + (1 << (bits >> 24)) - 1) & ~((1 << (bits >> 24)) - 1) if bits else start
        out.append((raw & 0x80FFFFFF, aligned, size - (aligned - start)))
        offset = start + size
        offset += (-offset) & 3
    return out


def triangles(primitive, indices):
    """Triangles of a GX primitive (0 triangles, 1 strip, 2 fan, 3 quads), front faces kept."""
    if primitive == 0:
        return [tuple(indices[i:i + 3]) for i in range(0, len(indices) - 2, 3)]
    if primitive == 1:
        return [(indices[i], indices[i + 1], indices[i + 2]) if i % 2 == 0
                else (indices[i + 1], indices[i], indices[i + 2]) for i in range(len(indices) - 2)]
    if primitive == 2:
        return [(indices[0], indices[i], indices[i + 1]) for i in range(1, len(indices) - 1)]
    if primitive == 3:
        out = []
        for i in range(0, len(indices) - 3, 4):
            out += [(indices[i], indices[i + 1], indices[i + 2]), (indices[i], indices[i + 2], indices[i + 3])]
        return out
    raise ValueError(f"unsupported primitive {primitive}")


class Corner:
    __slots__ = ("pos", "uv", "bones", "weights", "texture")

    def __init__(self, pos, uv, bones, weights, texture):
        self.pos, self.uv, self.bones, self.weights, self.texture = pos, uv, bones, weights, texture


class Model:
    """Triangle corners in bind pose (three per triangle) and the bind matrix of each bone."""

    def __init__(self):
        self.corners = []
        self.binds = {}


def _binds(blob):
    return {struct.unpack_from(">I", blob, i)[0]: struct.unpack_from(">16f", blob, i + 4)
            for i in range(0, len(blob) - 67, 68)}


def read_gamecube_model(path):
    """A GameCube character model (.glg). Each vertex follows one bone (its stitch index)."""
    data = open(path, "rb").read()
    root = _gc_chunks(data, 0, len(data))
    if not root or root[0][0] != 0x8001B000:
        raise ValueError(f"{path}: not a GameCube model")
    _, rstart, rsize = root[0]
    chunks, skin = {}, []
    for cid, start, size in _gc_chunks(data, rstart, rstart + rsize):
        chunks[cid] = data[start:start + size]
        if cid == 0x8001B008:
            skin = [(c, data[s:s + n]) for c, s, n in _gc_chunks(data, start, start + size)]
    models, packets, streams = chunks[0x1B003], chunks[0x1B004], chunks[0x1B005]
    vertices, indices = chunks[0x1B006], chunks[0x1B007]
    bone_maps = [blob for cid, blob in skin if cid == 0x1B00B][1:]  # the first map is the software one
    stitch = {}
    for cid, blob in skin:
        if cid == 0x1B010:
            stitch[struct.unpack_from(">i", blob, 4)[0]] = blob[8:]
    model = Model()
    model.binds = _binds(next(blob for cid, blob in skin if cid == 0x1B00A))
    total = sum(struct.unpack_from(">I", models, m * 16)[0] for m in range(len(models) // 16))
    packet_size = len(packets) // total
    if packet_size not in (0x42, 0x4A):  # older exports lack the material state word and two trailing words
        raise ValueError(f"{path}: unknown packet size {packet_size}")
    packet_index = 0
    for m in range(len(models) // 16):
        count, _, _, first = struct.unpack_from(">IIII", models, m * 16)
        for p in range(count):
            o = first + p * packet_size
            _, index_offset, nverts, primitive, nstreams, stream_offset = struct.unpack_from(">IIHBBI", packets, o)
            texture_offset = o + 0x10 + 8 + (4 if packet_size == 0x4A else 0) + 12
            texture = struct.unpack_from(">I", packets, texture_offset)[0]
            layout = {}
            for s in range(nstreams):
                address, sid, stride = struct.unpack_from(">IBB", streams, stream_offset + s * 6)
                layout[sid] = (address, stride)
            entries = list(struct.unpack_from(f">{nverts}H", indices, index_offset))
            bones = bone_maps[packet_index]
            register = {struct.unpack_from(">I", bones, j + 4)[0]: struct.unpack_from(">I", bones, j)[0]
                        for j in range(0, len(bones), 8)}
            for tri in triangles(primitive, list(range(nverts))):
                if len({entries[e] for e in tri}) < 3:
                    continue
                for e in tri:
                    i = entries[e]
                    address, stride = layout[0]
                    pos = (struct.unpack_from(">3f", vertices, address + i * 12) if stride == 12
                           else tuple(v / 256 for v in struct.unpack_from(">3h", vertices, address + i * 6)))
                    address, _ = layout[3]
                    uv = tuple(v / 1024 for v in struct.unpack_from(">2h", vertices, address + i * 4))
                    bone = register[stitch[packet_index][e]]
                    model.corners.append(Corner(pos, uv, (bone, 0, 0, 0), (1.0, 0.0, 0.0, 0.0), texture))
            packet_index += 1
    return model


def read_wii_model(path):
    """A Charged character model (.rlg) with up to four weighted bones per vertex."""
    data = open(path, "rb").read()
    cid, start, size = _wii_chunks(data, 0, len(data))[0]
    if cid != 0x8001B000:
        raise ValueError(f"{path}: not a single-group Wii model")
    chunks, bone_maps, model = {}, [], Model()
    for c, s, n in _wii_chunks(data, start, start + size):
        chunks[c] = data[s:s + n]
        if c == 0x8001B008:
            for c2, s2, n2 in _wii_chunks(data, s, s + n):
                if c2 == 0x1B00B:
                    bone_maps.append([struct.unpack_from(">I", data, s2 + j)[0] for j in range(0, n2, 4)])
                elif c2 == 0x1B00A:
                    model.binds = _binds(data[s2:s2 + n2])
    models, packets, streams = chunks[0x1B003], chunks[0x1B004], chunks[0x1B005]
    vertices, indices, parameters = chunks[0x1B006], chunks[0x1B007], chunks[0x1B016]
    packet_index = 0
    for m in range(len(models) // 12):
        count = struct.unpack_from(">I", models, m * 12 + 4)[0]
        for _ in range(count):
            o = packet_index * 48  # FixupModelData gives each model the next packets in order
            index_offset, nindices, _, primitive, nstreams, stream_offset = struct.unpack_from(">IIHBBI", packets, o)
            texture = struct.unpack_from(">I", parameters, struct.unpack_from(">I", packets, o + 32)[0])[0]
            layout = {}
            for s in range(nstreams):
                address, _, stride, sid, _ = struct.unpack_from(">IBBBB", streams, stream_offset + s * 8)
                layout.setdefault(sid, (address, stride))
            entries = list(struct.unpack_from(f">{nindices}H", indices, index_offset))
            for tri in triangles(primitive, entries):
                if len(set(tri)) < 3:
                    continue
                for i in tri:
                    address, stride = layout[1]
                    pos = struct.unpack_from(">3f", vertices, address + i * stride)
                    address, stride = layout[4]
                    uv = (struct.unpack_from(">2f", vertices, address + i * 8) if stride == 8
                          else tuple(v / 1024 for v in struct.unpack_from(">2h", vertices, address + i * 4)))
                    address, _ = layout[7]
                    slots = vertices[address + i * 4:address + i * 4 + 4]
                    address, _ = layout[5]
                    weights = struct.unpack_from(">4f", vertices, address + i * 16)
                    bones = tuple(bone_maps[packet_index][b] if w > 0 else 0 for b, w in zip(slots, weights))
                    model.corners.append(Corner(pos, uv, bones, weights, texture))
            packet_index += 1
    return model


def read_parents(path):
    """Bone -> parent bone (0 for the root) of a .shier; the same layout on both consoles."""
    data = open(path, "rb").read()
    chunks, offset = {}, 8
    while offset + 8 <= len(data):
        raw, size = struct.unpack_from(">II", data, offset)
        start = offset + 8
        if raw & 0x7F000000:
            align = 1 << ((raw & 0x7F000000) >> 24)
            start = (start + align - 1) & ~(align - 1)
        chunks[raw & 0x00FFFFFF] = data[start:start + size]
        offset = offset + 8 + size
    ids = [struct.unpack_from(">I", chunks[0x18003], i)[0] for i in range(0, len(chunks[0x18003]), 4)]
    parents = [struct.unpack_from(">i", chunks[0x18009], i)[0] for i in range(0, len(chunks[0x18009]), 4)]
    return {bone: ids[p] if p >= 0 else 0 for bone, p in zip(ids, parents)}


def write_mesh(path, model, parents):
    """The binary input of make_map.cpp (little endian)."""
    with open(path, "wb") as f:
        f.write(b"MSH2" + struct.pack("<I", len(model.corners) // 3))
        for c in model.corners:
            f.write(struct.pack("<3f2f4I4fI", *c.pos, *c.uv, *c.bones, *c.weights, c.texture))
        f.write(struct.pack("<I", len(model.binds)))
        for bone, matrix in sorted(model.binds.items()):
            f.write(struct.pack("<I16f", bone, *matrix))
        f.write(struct.pack("<I", len(parents)))
        for bone, parent in sorted(parents.items()):
            f.write(struct.pack("<II", bone, parent))


def _decode_cmpr(blob, width, height):
    """GX CMPR (8x8 tiles of four 4x4 blocks) to RGBA bytes."""
    def colour(v):
        return ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
    out = bytearray(width * height * 4)
    pos = 0
    for ty in range(0, height, 8):
        for tx in range(0, width, 8):
            for b in range(4):
                c0, c1, bits = struct.unpack_from(">HHI", blob, pos)
                pos += 8
                a, z = colour(c0), colour(c1)
                if c0 > c1:
                    palette = [a + (255,), z + (255,), tuple((2 * a[i] + z[i]) // 3 for i in range(3)) + (255,),
                               tuple((a[i] + 2 * z[i]) // 3 for i in range(3)) + (255,)]
                else:
                    palette = [a + (255,), z + (255,), tuple((a[i] + z[i]) // 2 for i in range(3)) + (255,), (0, 0, 0, 0)]
                for k in range(16):
                    x, y = tx + (b & 1) * 4 + k % 4, ty + (b >> 1) * 4 + k // 4
                    if x < width and y < height:
                        out[(y * width + x) * 4:(y * width + x) * 4 + 4] = bytes(palette[(bits >> (30 - 2 * k)) & 3])
    return out


def read_texture(path, wanted, gamecube=False):
    """Level 0 of CMPR texture `wanted` (name hash) from a .rlt (Wii) or .glt (GameCube) bundle.

    Both start with "PTLG" and the texture count; the directory follows at byte 16 on the Wii
    and at byte 32 on the GameCube, the textures right after it.
    """
    data = open(path, "rb").read()
    count = struct.unpack_from(">I", data, 4)[0]
    directory = 0x20 if gamecube else 16
    base = directory + 16 * count
    for i in range(count):
        name, offset, size, _ = struct.unpack_from(">IIII", data, directory + 16 * i)
        if name != wanted:
            continue
        texture = data[base + offset:base + offset + size]
        levels, fmt = struct.unpack_from(">II", texture, 0)
        width, height = struct.unpack_from(">HH", texture, 14)
        if fmt != 2:
            raise ValueError(f"{path}: texture {wanted:08x} is not CMPR")
        return width, height, _decode_cmpr(texture[32:], width, height)
    raise ValueError(f"{path}: no texture {wanted:08x}")


def name_hash(name):
    """nlStringHash, the hash of texture and bone names."""
    value = 0xFFFFFFFF
    for c in name.encode():
        value = (value + (value << 5) + c) & 0xFFFFFFFF
    return value
