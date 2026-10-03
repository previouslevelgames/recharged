"""Synthetic Wii RLG/RLT assets; no retail bytes or native pointer layouts."""
import struct


def make_assets(lit=False):
    chunks = []

    def chunk(kind, data):
        chunks.append(struct.pack(">II", kind, len(data)) + data + b"\0" * (-len(data) % 4))

    parameters = struct.pack(">IHBB", 0x12345678, 0, 3, 0)
    if lit:
        parameters += struct.pack(">ff5I", 0, 0, 1, 0, 1, 1, 0)
    chunk(0x1B016, parameters)
    chunk(0x1B007, struct.pack(">3H", 0, 1, 2))
    if lit:
        positions = struct.pack(">9f", -1, -.7, 0, 1, -.7, 0, 0, .9, 0)
        vertices = positions + bytes([0, 0, 64]) * 3 + struct.pack(">6h", 0, 1024, 1024, 1024, 512, 0) + bytes([255]) * 12
        chunk(0x1B006, vertices)
        chunk(0x1B005, b"".join(struct.pack(">IBBBB", *s) for s in
            [(0,0,12,1,0), (36,0,3,2,0), (45,0,4,4,0), (57,0,4,3,0)]))
    else:
        chunk(0x1B006, struct.pack(">15f", -1, -0.7, 0, 1, -0.7, 0, 0, 0.9, 0, 0, 1, 1, 1, 0.5, 0))
        chunk(0x1B005, struct.pack(">IBBBBIBBBB", 0, 0, 12, 1, 0, 36, 0, 8, 4, 0))
    packet = bytearray(48)
    struct.pack_into(">IIHBB", packet, 0, 0, 3, 3, 0, 4 if lit else 2)
    struct.pack_into(">I", packet, 16, 0x2169DB5C if lit else 0x21DB4385)
    struct.pack_into(">I", packet, 28, 0xC0007)  # depth test/write, LEQUAL, RGBA
    chunk(0x1B004, packet)
    chunk(0x1B002, struct.pack(">16f", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1))
    chunk(0x1B003, struct.pack(">III", 0x87654321, 1, 0))
    payload = b"".join(chunks)
    model = struct.pack(">II", 0x8001B000, len(payload)) + payload
    header = bytearray(32)
    struct.pack_into(">II", header, 0, 1, 3)  # one mip, RGBA8
    struct.pack_into(">HH", header, 14, 4, 4)
    # One GX 4x4 tile: AR followed by GB, with varying synthetic color.
    pixels = bytes(v for i in range(16) for v in (255, 240 if i % 2 else 20))
    pixels += bytes(v for i in range(16) for v in (70 if i % 2 else 190, 30))
    texture = struct.pack(">IIIIIIII", 0x50544C47, 1, 0, 0, 0x12345678, 0, 96, 0) + header + pixels
    return model, texture


def make_shadow():
    header = bytearray(32)
    struct.pack_into(">II", header, 0, 1, 8)
    struct.pack_into(">HH", header, 14, 8, 4)
    struct.pack_into(">I", header, 20, 2)
    entry = header + bytes(32) + bytes.fromhex("c210ffff")
    return struct.pack(">8I", 0x50544C47, 1, 0, 0, 0x5A5A5A5A, 0, len(entry), 0) + entry


def make_world():
    import zlib
    def chunk(kind, payload):
        return struct.pack(">II", kind, len(payload)) + payload + b"\0" * (-len(payload) % 4)
    points = [(-.5,-.5,.5),(.5,-.5,.5),(.5,.5,.5),(-.5,.5,.5),
              (-.5,-.5,-.5),(.5,-.5,-.5),(.5,.5,-.5),(-.5,.5,-.5)]
    indices = [0,1,2,0,2,3,4,6,5,4,7,6,0,5,1,0,4,5,3,6,7,3,2,6,0,7,4,0,3,7,1,6,2,1,5,6]
    packet = bytearray(48)
    struct.pack_into(">IIHBB", packet, 0, 0, len(indices), 8, 0, 3)
    struct.pack_into(">I", packet, 16, 0x386ECBDD)
    vertices = b"".join(struct.pack(">3f", *p) for p in points) + bytes([255])*32 + bytes(64)
    streams = b"".join(struct.pack(">IBBBB", *s) for s in [(0,0,12,1,0),(96,0,4,3,0),(128,0,8,4,0)])
    parts = [(0x1B016,struct.pack(">IHBBI",0x12345678,0xffff,3,0,1)),
             (0x1B007,struct.pack(">36H",*indices)),(0x1B006,vertices),(0x1B005,streams),
             (0x1B004,packet),(0x1B002,struct.pack(">16f",1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)),
             (0x1B003,struct.pack(">III",0x87654321,1,0))]
    model = chunk(0x8001B000,b"".join(chunk(k,v) for k,v in parts))
    _, texture = make_assets()
    raw = chunk(0x80000001,chunk(0x24100,texture)+chunk(0x8001B100,model))
    return struct.pack(">I",len(raw))+zlib.compress(raw)


def make_specular_world(missing_gloss=False):
    """Four distinct material bindings in a compressed synthetic world."""
    import zlib

    def chunk(kind, payload):
        return struct.pack(">II", kind, len(payload)) + payload + b"\0" * (-len(payload) % 4)

    parameters = b"".join(struct.pack(">IHBB", 0x12345678 + i, 0, 3, 0) for i in range(4))
    parameters += struct.pack(">7f2I", .5, .5, 64, 1, .5, .25, 1, 1, 0)
    vertices = struct.pack(">9f", -1, -.7, 0, 1, -.7, 0, 0, .9, 0) + bytes([0,0,64]) * 3
    vertices += struct.pack(">6h", 128,128,128,128,128,128) * 4 + bytes([255]) * 12
    streams = b"".join(struct.pack(">IBBBB", offset, 0, stride, kind, 0) for offset,stride,kind in
        [(0,12,1),(36,3,2),(45,4,4),(57,4,4),(69,4,4),(81,4,4),(93,4,3)])
    packet = bytearray(48)
    struct.pack_into(">IIHBB", packet, 0, 0, 3, 3, 0, 7)
    struct.pack_into(">I", packet, 16, 0x112AB470)
    struct.pack_into(">I", packet, 28, 0xC0007)
    parts = [(0x1B016,parameters),(0x1B007,struct.pack(">3H",0,1,2)),
             (0x1B006,vertices),(0x1B005,streams),(0x1B004,packet),
             (0x1B002,struct.pack(">16f",1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)),
             (0x1B003,struct.pack(">III",0x87654321,1,0))]
    model = chunk(0x8001B000,b"".join(chunk(k,v) for k,v in parts))
    count = 3 if missing_gloss else 4
    header = bytearray(32)
    struct.pack_into(">II", header, 0, 1, 3)
    struct.pack_into(">HH", header, 14, 4, 4)
    entries = []
    for r,g,b in [(80,100,120),(200,40,20),(128,128,128),(128,64,32)][:count]:
        entries.append(header + bytes([255,r]) * 16 + bytes([g,b]) * 16)
    table = b"".join(struct.pack(">4I",0x12345678+i,i*96,96,0) for i in range(count))
    textures = struct.pack(">4I",0x50544C47,count,0,0) + table + b"".join(entries)
    raw = chunk(0x80000001,chunk(0x24100,textures)+chunk(0x8001B100,model))
    return struct.pack(">I",len(raw))+zlib.compress(raw)


def make_scrolling_specular_world(missing_specular=False):
    """Two distinct scrolling material bindings in a compressed synthetic world."""
    import zlib

    def chunk(kind, payload):
        return struct.pack(">II", kind, len(payload)) + payload + b"\0" * (-len(payload) % 4)

    parameters = b"".join(struct.pack(">IHBB", 0x12345678 + i, 0, 3, 0) for i in range(2))
    parameters += struct.pack(">8f3I", .5, 64, 1, .5, .25, 1, .25, -.5, 1, 1, 0)
    vertices = struct.pack(">9f", -1, -.7, 0, 1, -.7, 0, 0, .9, 0) + bytes([0,0,64]) * 3
    vertices += struct.pack(">6h", 128,128,128,128,128,128) * 2 + bytes([255]) * 12
    streams = b"".join(struct.pack(">IBBBB", offset, 0, stride, kind, 0) for offset,stride,kind in
        [(0,12,1),(36,3,2),(45,4,4),(57,4,4),(69,4,3)])
    packet = bytearray(48)
    struct.pack_into(">IIHBB", packet, 0, 0, 3, 3, 0, 5)
    struct.pack_into(">I", packet, 16, 0x3ECCD955)
    struct.pack_into(">I", packet, 28, 0xC0007)
    parts = [(0x1B016,parameters),(0x1B007,struct.pack(">3H",0,1,2)),
             (0x1B006,vertices),(0x1B005,streams),(0x1B004,packet),
             (0x1B002,struct.pack(">16f",1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)),
             (0x1B003,struct.pack(">III",0x87654321,1,0))]
    model = chunk(0x8001B000,b"".join(chunk(k,v) for k,v in parts))
    count = 1 if missing_specular else 2
    header = bytearray(32)
    struct.pack_into(">II", header, 0, 1, 3)
    struct.pack_into(">HH", header, 14, 4, 4)
    entries = []
    for r,g,b in [(80,100,120),(200,40,20),(128,128,128),(128,64,32)][:count]:
        entries.append(header + bytes([255,r]) * 16 + bytes([g,b]) * 16)
    table = b"".join(struct.pack(">4I",0x12345678+i,i*96,96,0) for i in range(count))
    textures = struct.pack(">4I",0x50544C47,count,0,0) + table + b"".join(entries)
    raw = chunk(0x80000001,chunk(0x24100,textures)+chunk(0x8001B100,model))
    return struct.pack(">I",len(raw))+zlib.compress(raw)


def make_camera_overlay_world(missing_mask=False):
    """Three camera overlay material bindings in a compressed synthetic world."""
    import zlib

    def chunk(kind, payload):
        return struct.pack(">II", kind, len(payload)) + payload + b"\0" * (-len(payload) % 4)

    parameters = b"".join(struct.pack(">IHBB", 0x12345678 + i, 0, 3, 0) for i in range(3))
    parameters += struct.pack(">3f3I", 2, 1, .5, 1, 1, 0)
    vertices = struct.pack(">9f", -1, -.7, 0, 1, -.7, 0, 0, .9, 0) + bytes([0,0,64]) * 3
    vertices += struct.pack(">6h", 128,128,128,128,128,128) * 3 + bytes([255]) * 12
    streams = b"".join(struct.pack(">IBBBB", offset, 0, stride, kind, 0) for offset,stride,kind in
        [(0,12,1),(36,3,2),(45,4,4),(57,4,4),(69,4,4),(81,4,3)])
    packet = bytearray(48)
    struct.pack_into(">IIHBB", packet, 0, 0, 3, 3, 0, 6)
    struct.pack_into(">I", packet, 16, 0x32BC21E8)
    struct.pack_into(">I", packet, 28, 0xC0007)
    parts = [(0x1B016,parameters),(0x1B007,struct.pack(">3H",0,1,2)),
             (0x1B006,vertices),(0x1B005,streams),(0x1B004,packet),
             (0x1B002,struct.pack(">16f",1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)),
             (0x1B003,struct.pack(">III",0x87654321,1,0))]
    model = chunk(0x8001B000,b"".join(chunk(k,v) for k,v in parts))
    count = 2 if missing_mask else 3
    header = bytearray(32)
    struct.pack_into(">II", header, 0, 1, 3)
    struct.pack_into(">HH", header, 14, 4, 4)
    entries = []
    for r,g,b in [(80,100,120),(200,40,20),(128,128,128),(128,64,32)][:count]:
        entries.append(header + bytes([255,r]) * 16 + bytes([g,b]) * 16)
    table = b"".join(struct.pack(">4I",0x12345678+i,i*96,96,0) for i in range(count))
    textures = struct.pack(">4I",0x50544C47,count,0,0) + table + b"".join(entries)
    raw = chunk(0x80000001,chunk(0x24100,textures)+chunk(0x8001B100,model))
    return struct.pack(">I",len(raw))+zlib.compress(raw)


def make_masked_detail_world(missing_mask=False):
    """Three masked detail material bindings in a compressed synthetic world."""
    import zlib

    def chunk(kind, payload):
        return struct.pack(">II", kind, len(payload)) + payload + b"\0" * (-len(payload) % 4)

    parameters = b"".join(struct.pack(">IHBB", 0x12345678 + i, 0, 3, 0) for i in range(3))
    parameters += struct.pack(">f2I", .5, 1, 0)
    vertices = struct.pack(">9f", -1, -.7, 0, 1, -.7, 0, 0, .9, 0) + bytes([0,0,64]) * 3
    vertices += struct.pack(">6h", 128,128,128,128,128,128) * 3 + bytes([255]) * 12
    streams = b"".join(struct.pack(">IBBBB", offset, 0, stride, kind, 0) for offset,stride,kind in
        [(0,12,1),(36,3,2),(45,4,4),(57,4,4),(69,4,4),(81,4,3)])
    packet = bytearray(48)
    struct.pack_into(">IIHBB", packet, 0, 0, 3, 3, 0, 6)
    struct.pack_into(">I", packet, 16, 0x09609A35)
    struct.pack_into(">I", packet, 28, 0xC0007)
    parts = [(0x1B016,parameters),(0x1B007,struct.pack(">3H",0,1,2)),
             (0x1B006,vertices),(0x1B005,streams),(0x1B004,packet),
             (0x1B002,struct.pack(">16f",1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)),
             (0x1B003,struct.pack(">III",0x87654321,1,0))]
    model = chunk(0x8001B000,b"".join(chunk(k,v) for k,v in parts))
    count = 2 if missing_mask else 3
    header = bytearray(32)
    struct.pack_into(">II", header, 0, 1, 3)
    struct.pack_into(">HH", header, 14, 4, 4)
    entries = []
    for r,g,b in [(80,100,120),(200,40,20),(128,128,128),(128,64,32)][:count]:
        entries.append(header + bytes([255,r]) * 16 + bytes([g,b]) * 16)
    table = b"".join(struct.pack(">4I",0x12345678+i,i*96,96,0) for i in range(count))
    textures = struct.pack(">4I",0x50544C47,count,0,0) + table + b"".join(entries)
    raw = chunk(0x80000001,chunk(0x24100,textures)+chunk(0x8001B100,model))
    return struct.pack(">I",len(raw))+zlib.compress(raw)


def make_scrolling_masked_detail_world(missing_mask=False, animated=False, missing_frame=False):
    """Three masked detail material bindings in a compressed synthetic world."""
    import zlib

    def chunk(kind, payload):
        return struct.pack(">II", kind, len(payload)) + payload + b"\0" * (-len(payload) % 4)

    parameters = b"".join(struct.pack(">IHBB", 0x12345678 + i, 0, 3, 0) for i in range(3))
    parameters += struct.pack(">7f2I", .5, -.25, -.5, .25, .75, -.75, .5, 1, 0)
    vertices = struct.pack(">9f", -1, -.7, 0, 1, -.7, 0, 0, .9, 0) + bytes([0,0,64]) * 3
    vertices += struct.pack(">6h", 128,128,128,128,128,128) * 3 + bytes([255]) * 12
    streams = b"".join(struct.pack(">IBBBB", offset, 0, stride, kind, 0) for offset,stride,kind in
        [(0,12,1),(36,3,2),(45,4,4),(57,4,4),(69,4,4),(81,4,3)])
    packet = bytearray(48)
    struct.pack_into(">IIHBB", packet, 0, 0, 3, 3, 0, 6)
    struct.pack_into(">I", packet, 16, 0xF2D57AC6)
    struct.pack_into(">I", packet, 28, 0xC0007)
    parts = [(0x1B016,parameters),(0x1B007,struct.pack(">3H",0,1,2)),
             (0x1B006,vertices),(0x1B005,streams),(0x1B004,packet),
             (0x1B002,struct.pack(">16f",1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)),
             (0x1B003,struct.pack(">III",0x87654321,1,0))]
    model = chunk(0x8001B000,b"".join(chunk(k,v) for k,v in parts))
    count = 2 if missing_mask else 3
    header = bytearray(32)
    struct.pack_into(">II", header, 0, 1, 3)
    struct.pack_into(">HH", header, 14, 4, 4)
    entries = []
    for r,g,b in [(80,100,120),(200,40,20),(128,128,128),(128,64,32)][:count]:
        entries.append(header + bytes([255,r]) * 16 + bytes([g,b]) * 16)
    table = b"".join(struct.pack(">4I",0x12345678+i,i*96,96,0) for i in range(count))
    textures = struct.pack(">4I",0x50544C47,count,0,0) + table + b"".join(entries)
    if animated: textures = animate_texture_bundle(textures, missing_frame)
    raw = chunk(0x80000001,chunk(0x24100,textures)+chunk(0x8001B100,model))
    return struct.pack(">I",len(raw))+zlib.compress(raw)


def make_scrolling_camera_world(missing_mask=False):
    """Three camera overlay material bindings in a compressed synthetic world."""
    import zlib

    def chunk(kind, payload):
        return struct.pack(">II", kind, len(payload)) + payload + b"\0" * (-len(payload) % 4)

    parameters = b"".join(struct.pack(">IHBB", 0x12345678 + i, 0, 3, 0) for i in range(3))
    parameters += struct.pack(">3f2I2f2I", 2, 1, .5, 1, 1, .5, -.25, 0, 0)
    vertices = struct.pack(">9f", -1, -.7, 0, 1, -.7, 0, 0, .9, 0) + bytes([0,0,64]) * 3
    vertices += struct.pack(">6h", 128,128,128,128,128,128) * 3 + bytes([255]) * 12
    streams = b"".join(struct.pack(">IBBBB", offset, 0, stride, kind, 0) for offset,stride,kind in
        [(0,12,1),(36,3,2),(45,4,4),(57,4,4),(69,4,4),(81,4,3)])
    packet = bytearray(48)
    struct.pack_into(">IIHBB", packet, 0, 0, 3, 3, 0, 6)
    struct.pack_into(">I", packet, 16, 0x845CAD59)
    struct.pack_into(">I", packet, 28, 0xC0007)
    parts = [(0x1B016,parameters),(0x1B007,struct.pack(">3H",0,1,2)),
             (0x1B006,vertices),(0x1B005,streams),(0x1B004,packet),
             (0x1B002,struct.pack(">16f",1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)),
             (0x1B003,struct.pack(">III",0x87654321,1,0))]
    model = chunk(0x8001B000,b"".join(chunk(k,v) for k,v in parts))
    count = 2 if missing_mask else 3
    header = bytearray(32)
    struct.pack_into(">II", header, 0, 1, 3)
    struct.pack_into(">HH", header, 14, 4, 4)
    entries = []
    for r,g,b in [(80,100,120),(200,40,20),(128,128,128),(128,64,32)][:count]:
        entries.append(header + bytes([255,r]) * 16 + bytes([g,b]) * 16)
    table = b"".join(struct.pack(">4I",0x12345678+i,i*96,96,0) for i in range(count))
    textures = struct.pack(">4I",0x50544C47,count,0,0) + table + b"".join(entries)
    raw = chunk(0x80000001,chunk(0x24100,textures)+chunk(0x8001B100,model))
    return struct.pack(">I",len(raw))+zlib.compress(raw)


def animate_texture_bundle(bundle, missing_frame=False):
    """Replace the first static ID with an IFL alias to two synthetic images."""
    count = struct.unpack_from(">I", bundle, 4)[0]
    start = 16 + count * 16
    records = []
    for i in range(count):
        key, offset, size, _ = struct.unpack_from(">4I", bundle, 16 + i * 16)
        records.append((key, bundle[start + offset:start + offset + size]))
    key, image = records[0]
    other = bytearray(image)
    for i in range(32, len(other)):
        if i % 2: other[i] = 255 - other[i]
    anim = struct.pack(">IIiiiB3xIfI", 0x5f6c6669, key, 2, 0, 0, 0, 0xffff, 0, 0)
    anim += struct.pack(">IfIf", 0xabc00001, .05, 0xabc00002, .05)
    records = [(key, anim), *records[1:], (0xabc00001, image)]
    if not missing_frame: records.append((0xabc00002, other))
    table, body = bytearray(), bytearray()
    for key, data in records:
        table += struct.pack(">4I", key, len(body), len(data), 0)
        body += data
    return struct.pack(">4I", 0x50544c47, len(records), 0, 0) + table + body
