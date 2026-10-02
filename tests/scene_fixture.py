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
