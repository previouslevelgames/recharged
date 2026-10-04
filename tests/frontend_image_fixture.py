"""Independent big-endian BundleFile and PlatTexture test encoding."""
import struct


def texture(fmt=1, levels=1, width=8, height=8, value=65):
    header = bytearray(32)
    struct.pack_into(">II", header, 0, levels, fmt)
    header[8:12] = bytes((1, 2, 3, 4))
    struct.pack_into(">HH", header, 14, width, height)
    palette = 16 if fmt == 8 else 0
    struct.pack_into(">I", header, 20, palette)
    # Deliberately poisonous saved addresses: no native loader may borrow them.
    struct.pack_into(">II", header, 24, 0xF00DBAAD, 0xDEADBEEF)
    block_w, block_h, block_bytes = ((4, 4, 32), (4, 4, 32), (8, 8, 32),
        (4, 4, 64), (8, 4, 32), (8, 8, 32), (8, 4, 32), (4, 4, 32), (8, 4, 32))[fmt]
    size = sum(((max(1, width >> i) + block_w - 1) // block_w)
               * ((max(1, height >> i) + block_h - 1) // block_h) * block_bytes for i in range(levels))
    pixels = bytes((value + i) % (16 if palette else 256) for i in range(size))
    colours = b"".join(struct.pack(">H", 0x8000 | (i * 1027 & 0x7FFF)) for i in range(palette))
    return bytes(header) + pixels + colours


def bundle(entries):
    # Empty original files have only16 header bytes although both offsets point32.
    directory, data = 32, (32 + len(entries) * 12 + 31) // 32 * 32
    if not entries:
        return struct.pack(">4I", 32, 0, 1, 1)
    result = bytearray(data)
    struct.pack_into(">4I", result, 0, 32, len(entries), directory // 32, data // 32)
    for i, (identity, payload) in enumerate(entries):
        struct.pack_into(">3I", result, directory + i * 12, identity, len(result) // 32, len(payload))
        result.extend(payload)
        result.extend(bytes(-len(result) % 32))
    return bytes(result)


def files():
    return {"art/fe/MainUI.Dmn": bundle([(0x11, texture(value=65)),
                (0x11, texture(value=66)), (0x33, texture(2, value=90))]),
            "art/fe/InGameUI.Res": bundle([(0x11, texture(3, value=80))]),
            "art/fe/InGameUI.Dmn": bundle([(0x11, b"unused duplicate"), (0x22, texture(8, value=3))]),
            "art/fe/BootLoadingUI.res": bundle([(0x11, texture(value=127)), (0x44, texture(8, value=7))])}
