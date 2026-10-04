"""Independent Wii-width retarget inventory generator; no game data."""
import random
import struct


def retarget_fixture(seed, lists=None):
    if lists is None:
        lists = [[(0x12340000 + seed, -0x70000000 + seed, [2, -1, 0]),
                  (0x12340000 + seed, 1, [1, 2, 0]), (0x87650000 + seed, 0, [0, 1])]]
    data = bytearray()
    rng = random.Random(seed)

    def begin(kind):
        exponent = rng.randrange(6)
        at = len(data)
        data.extend(struct.pack(">II", kind | exponent << 24, 0))
        data.extend(b"\xaa" * (-len(data) % (1 << exponent)))
        return at

    def end(at):
        struct.pack_into(">I", data, at + 4, len(data) - at - 8)
        data.extend(b"\xcd" * (-len(data) % 4))

    def chunk(kind, payload):
        at = begin(kind)
        data.extend(payload)
        end(at)

    for index, maps in enumerate(lists):
        root = begin(0x80017104)
        chunk(0x17105, struct.pack(">IIII", 0xfedcba98, 0x76540000 + index, len(maps), 0xccccbbbb))
        group = begin(0x80017106)
        chunk(0x17107, b"".join(struct.pack(">IIII", signature, metadata & 0xffffffff, len(nodes), 0xcafebabe)
                                for signature, metadata, nodes in maps))
        for signature, metadata, nodes in maps:
            values = b"".join(struct.pack(">h", node) for node in nodes)
            # Deliberately invalid as a node: padding must never be interpreted.
            if len(nodes) % 2:
                values += b"\xff\xfe"
            chunk(0x17108, values)
        end(group)
        end(root)
    return bytes(data)


def chunks(data, begin=0, end=None):
    if end is None:
        end = len(data)
    while begin < end:
        raw, size = struct.unpack_from(">II", data, begin)
        alignment = 2 ** ((raw >> 24) & 127)
        start = ((begin + 8 + alignment - 1) // alignment) * alignment
        stop = begin + 8 + size
        yield raw & 0x80ffffff, start, stop, begin
        begin = (stop + 3) // 4 * 4


def retarget_oracle(data):
    result = []
    for _, start, stop, _ in chunks(data):
        children = list(chunks(data, start, stop))
        header = children[0][1]
        _, identity, count, _ = struct.unpack_from(">IIII", data, header)
        nested = list(chunks(data, children[1][1], children[1][2]))
        maps = []
        for i in range(count):
            signature, metadata, length, _ = struct.unpack_from(">IiII", data, nested[0][1] + i * 16)
            nodes = list(struct.unpack_from(">" + "h" * length, data, nested[i + 1][1]))
            maps.append((signature, metadata, nodes))
        result.append((identity, maps))
    return result
