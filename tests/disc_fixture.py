"""Original synthetic Wii disc fixture; contains no retail data."""
import struct


def write_disc(path, game_id=b"R4QE01", partition=True):
    """A tiny, unencrypted Wii container with one original text file."""
    data = bytearray(0x60000)
    data[:6] = game_id
    data[7] = 1
    data[0x18:0x1C] = bytes.fromhex("5d1c9ea3")
    title = b"Port test fixture"
    data[0x20:0x20 + len(title)] = title
    data[0x60:0x62] = b"\x01\x01"  # No hashes or encryption.
    if partition:
        struct.pack_into(">II", data, 0x40000, 1, 0x40020 >> 2)
        struct.pack_into(">II", data, 0x40020, 0x50000 >> 2, 0)
        issuer = b"Root-CA00000001-XS00000003"
        data[0x50140:0x50140 + len(issuer)] = issuer
        struct.pack_into(">II", data, 0x502B8, 0x8000 >> 2, 0x8000 >> 2)
        base = 0x58000
        data[base:base + 0x400] = data[:0x400]
        struct.pack_into(">III", data, base + 0x420, 0x2800 >> 2, 0x3000 >> 2, 36 >> 2)
        struct.pack_into(">I", data, base + 0x2800, 0x100)  # Synthetic DOL text offset.
        struct.pack_into(">I", data, base + 0x2800 + 0x90, 4)  # Text size.
        struct.pack_into(">III", data, base + 0x3000, 0x01000000, 0, 2)
        payload = b"Synthetic fixture data.\n"
        struct.pack_into(">III", data, base + 0x300C, 0, 0x3200 >> 2, len(payload))
        data[base + 0x3018:base + 0x3021] = b"test.txt\0"
        data[base + 0x3200:base + 0x3200 + len(payload)] = payload
    path.write_bytes(data)
