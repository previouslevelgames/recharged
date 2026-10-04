"""Generated NLOC/NLG/CI8 resources; no retail bytes."""
import struct


def name_hash(name):
    result = 0xffffffff
    for byte in name.encode():
        result = (result * 33 + byte) & 0xffffffff
    return result


def localization(language):
    return struct.pack(">7I", 0x4e4c4f43, 1, language, 1, 1, 1, 0) + "AB\0".encode("utf-16-be")


def font(base):
    description = ("NLG Font Description file\r\nVersion 1.1\r\n"
        "PageSize 32 PageCount 1 TexType color Distribution english\r\n"
        "Height 12 RenderHeight 16 Ascent 9 RenderAscent 11 IL 1\r\n"
        "CharSpacing 100 LineHeight 100\r\n"
        "Glyph ? Width 8 8 0\r\nGlyph A Width 10 8 0\r\nGlyph B Width 12 8 0\r\nEND\r\n").encode()
    texture = bytearray(32 + 1024 + 512)
    struct.pack_into(">II", texture, 0, 1, 8)
    struct.pack_into(">HH", texture, 14, 32, 32)
    struct.pack_into(">I", texture, 20, 256)
    texture[32:1056] = b"\x01" * 1024
    texture[1058:1060] = b"\xff\xff"
    data = bytearray(64)
    struct.pack_into(">4I", data, 0, 32, 2, 1, 2)
    struct.pack_into(">3I", data, 32, name_hash(base + "_1"), 2, len(texture))
    struct.pack_into(">3I", data, 44, name_hash(base), (64 + len(texture)) // 32, len(description))
    return data + texture + description


def files():
    result = {"Art/fe/" + name + ".loc": localization(language) for name, language in
        (("english", 0x7a947b29), ("nafrench", 0x30d469c4), ("naspanish", 0x2f242024))}
    result.update({"Art/fe/fonts/" + name + ".res": font("fe/fonts/" + name)
        for name in ("eurfonttext18", "eurfontheading36")})
    return result
