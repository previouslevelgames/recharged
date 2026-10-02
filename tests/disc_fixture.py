"""Original synthetic Wii disc fixture; contains no retail data."""
import struct


def write_disc(path, game_id=b"R4QE01", partition=True, files=None):
    """A tiny, unencrypted Wii container with synthetic files and directories."""
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
        struct.pack_into(">I", data, base + 0x2800, 0x100)  # Synthetic DOL text offset.
        struct.pack_into(">I", data, base + 0x2800 + 0x90, 4)  # Text size.
        if files is None:
            files = {"test.txt": b"Synthetic fixture data.\n"}
        tree = {}
        for name, payload in files.items():
            components = name.split("/")
            if any(part in ("", ".", "..") for part in components):
                raise ValueError("Invalid fixture path")
            directory = tree
            for part in components[:-1]:
                directory = directory.setdefault(part, {})
            directory[components[-1]] = payload
        entries, names = [], bytearray()
        position = 0x3200

        def emit(name, node, parent):
            nonlocal position
            index = len(entries)
            name_offset = len(names)
            if name:
                names.extend(name.encode("utf-8") + b"\0")
            if isinstance(node, dict):
                entries.append([0x01000000 | name_offset, parent, 0])
                for child, value in node.items():
                    emit(child, value, index)
                entries[index][2] = len(entries)
            else:
                entries.append([name_offset, position >> 2, len(node)])
                end = position + len(node)
                if end > 0x8000:
                    raise ValueError("Fixture payload exceeds its tiny partition")
                data[base + position:base + end] = node
                position = (end + 31) & ~31

        emit("", tree, 0)
        fst = b"".join(struct.pack(">III", *entry) for entry in entries) + names
        fst += b"\0" * (-len(fst) % 4)
        if len(fst) > 0x200:
            raise ValueError("Fixture FST exceeds its reserved space")
        struct.pack_into(">III", data, base + 0x420, 0x2800 >> 2, 0x3000 >> 2, len(fst) >> 2)
        data[base + 0x3000:base + 0x3000 + len(fst)] = fst
    path.write_bytes(data)
