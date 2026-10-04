"""Small generated FENL text graph. No original game data."""
import struct
from frontend_visual_fixture import name_hash


def layout():
    data = bytearray(0x280)
    pointers = []

    def word(at, value):
        struct.pack_into(">I", data, at, value)

    def pointer(at, value):
        word(at, value)
        pointers.append(at)

    def ring(at):
        pointer(at, at)
        pointer(at + 4, at)

    def attributes(at):
        struct.pack_into(">3f", data, at + 24, 1, 1, 1)
        data[at + 48:at + 53] = bytes([1, 255, 255, 255, 255])

    pointer(4, 0x18)
    pointer(8, 0x240)
    pointer(12, 0x1a0)
    word(16, 123)
    word(20, 1)
    pointer(0x18, 0x30)
    pointer(0x1c, 0x30)
    ring(0x30)
    pointer(0x38, 0x80)
    data[0x50:0x55] = b"Slide"
    ring(0x80)
    pointer(0x8c, 0x1a0)
    data[0x98:0x9d] = b"Label"
    attributes(0xbc)
    word(0x108, 3)
    data[0x10e] = 1
    word(0x110, 1)  # localization ID
    word(0x120, 8)  # stored localization override
    ring(0x1a0)
    attributes(0x1a8)
    data[0x1f4:0x1f8] = b"Text"
    word(0x214, 2)
    pointer(0x218, 0x240)
    ring(0x240)
    word(0x248, 1)
    word(0x24c, name_hash("fot-rodinprob18"))
    table = b"".join(struct.pack(">I", value) for value in pointers)
    return struct.pack(">4I", 0x46454e4c, 1, len(data), len(table)) + data + table
