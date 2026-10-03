"""Original synthetic CAM fixture with known big-endian values and aligned chunks."""
import struct


def camera_fixture():
    data = bytearray(struct.pack(">II", 0x8002500b, 0))

    def chunk(tag, payload, alignment=8):
        start = len(data)
        data.extend(struct.pack(">II", tag | ((alignment.bit_length()-1) << 24), 0))
        data.extend(b"\0" * (-len(data) % alignment))
        data.extend(payload)
        struct.pack_into(">I", data, start+4, len(data)-start-8)
        data.extend(b"\0" * (-len(data) % 4))

    name = b"fixture_camera_longer_than_thirty_two_bytes\0"
    chunk(0x25000, name + b"\0" * (-len(name) % 4))
    chunk(0x2500c, struct.pack(">I", 3))
    chunk(0x25003, struct.pack(">9f", 0, -2, 4, 1, -2, 4, 2, -2, 4))
    chunk(0x25006, struct.pack(">9f", *([7, 8, 9] * 3)))
    chunk(0x25004, struct.pack(">12f", *([0, 0, 0, 1] * 3)))
    chunk(0x25009, struct.pack(">3f", 40, 41, 42))
    chunk(0x2500a, struct.pack(">3f", 2, 3, 4))
    struct.pack_into(">I", data, 4, len(data)-8)
    return bytes(data)
