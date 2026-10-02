"""Synthetic Wii RLG/RLT assets; no retail bytes or native pointer layouts."""
import struct


def make_assets():
    chunks = []

    def chunk(kind, data):
        chunks.append(struct.pack(">II", kind, len(data)) + data + b"\0" * (-len(data) % 4))

    chunk(0x1B016, struct.pack(">IHBB", 0x12345678, 0, 3, 0))
    chunk(0x1B007, struct.pack(">3H", 0, 1, 2))
    chunk(0x1B006, struct.pack(">15f", -1, -0.7, 0, 1, -0.7, 0, 0, 0.9, 0, 0, 1, 1, 1, 0.5, 0))
    chunk(0x1B005, struct.pack(">IBBBBIBBBB", 0, 0, 12, 1, 0, 36, 0, 8, 4, 0))
    packet = bytearray(48)
    struct.pack_into(">IIHBB", packet, 0, 0, 3, 3, 0, 2)
    struct.pack_into(">I", packet, 16, 0x21DB4385)
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
