"""Small authored boot bytecode fixture; no original game data."""
import struct


def boot_script_fixture():
    name_hash = 0xFFFFFFFF
    for value in b"BootLoadingToFE":
        name_hash = (name_hash * 33 + value) & 0xFFFFFFFF
    # One zero-argument function, one numeric global, data word and a string.
    header = struct.pack(">18I", 0xE11C2112, 1, 0, 4, 4, 4, 8,
                         1, 1, 1, 1, 0, *([0] * 6))
    function = struct.pack(">IIHBB", name_hash, 0, 2, 0, 0)
    return header + function + struct.pack(">IIHH", 42, 0xCAFEBEEF, (8 << 11) | 11, 10 << 11) + b"loading\0"
