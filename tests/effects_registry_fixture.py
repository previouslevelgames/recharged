"""Independent disk encoding for retained original effects registration."""
import struct


def words(*values):
    return b"".join(struct.pack(">I", value & 0xffffffff) for value in values)


def chunk(kind, data):
    return words(kind, len(data)) + data + bytes(-len(data) % 4)


def template(identity, colours, persistent=0, texture=0x12345678, model=0xffffffff):
    data = bytearray(0x78 + colours * 4)
    struct.pack_into(">If", data, 0, identity, 1e10 if persistent == 1 else 4)
    for at in range(8, 0x34, 4):
        struct.pack_into(">f", data, at, (at - 20) / 4)
    struct.pack_into(">f", data, 0x10, 1000 if persistent == 2 else 8)
    data[0x34:0x38] = bytes((3, 1, 2, 10))
    struct.pack_into(">IIIIIffI", data, 0x38, texture, 3, 0xdeadf00d, 0xface4321, 0xaabbccee, 30, 2, model)
    data[0x58:0x78] = words(*([0xdeadbeef] * 8))
    data[0x78:] = bytes(range(colours * 4))
    pieces = chunk(0x24003, data)
    for i in range(8):
        prop = words(i == 1, 0x7fc00000 if i == 1 else 0x40600000,
                     0x7fc00000 if i == 1 else 0xc0000000, 2 if i == 1 else 0xdeadbeef, 0xfeedcafe)
        body = chunk(0x24005, prop)
        if i == 1:
            body += chunk(0x24006, struct.pack(">10f", 0, 2, -3, 4, 5, .5, 3, -3, 4, 5))
        pieces += chunk(0x80024004, body)
    return chunk(0x80024002, pieces)


def group(identity, user=False, binding=3, indices=(1, 0)):
    header = words(identity, 0xdeadbeef, len(indices), 1, 0xdeadbeef, user, 0xdeadbeef)
    specs = bytearray()
    for i, index in enumerate(indices):
        data = bytearray(88)
        struct.pack_into(">4IfIfIII4fIffIi", data, 0,
            0x10 + i, index, binding, 42, .25, 0, 1.5, 1, 1, 0,
            -2, .5, 1.5, 2.5, 9, -1, 2, 3, -1)
        specs.extend(data)
    source = chunk(0x24023, words(4, 0xdeadcafe) if user else b"")
    if user:
        source += chunk(0x24024, b"test")
    return chunk(0x80024020, chunk(0x24021, header) + chunk(0x24022, specs) + source)


def entry(base=10, group_hash=0x81f2a311, user=False, binding=3, persistent=0, texture=0x12345678, model=0xffffffff):
    head = chunk(0x24001, words(0xabcdef01, 2, 2, 0xdeadbeef, 1, 0xdeadbeef))
    head += chunk(0x24025, words(0xdeadbeef, 0xfeedcafe)) + chunk(0x24026, words(0xdeadbeef))
    return chunk(0x80024000, head + template(base, 25, persistent, texture, model)
        + template(base + 1, 26, texture=texture, model=model) + group(group_hash, user, binding))


def resident(*entries):
    return chunk(0x80000001, b"".join(entries))


def textures(identity=0x12345678, marker=64):
    header = bytearray(32)
    struct.pack_into(">II", header, 0, 1, 5)
    struct.pack_into(">HH", header, 14, 8, 8)
    data = bytes(header) + bytes((marker + i) % 256 for i in range(32))
    return words(0x50544c47, 1, 0, 0) + words(identity, 0, len(data), 0) + data


def files():
    return {"resident.bun": resident(entry()),
            "nonresident.bun": chunk(0x80000001, chunk(0x24100, textures())),
            "geometry.bun": b"opaque geometry requires qualified GL registration",
            "textures.rlt": textures(0x87654321)}
