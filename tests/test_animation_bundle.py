#!/usr/bin/env python3
"""Synthetic ordered world associations; all disk encodings are independent."""
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import zlib
from disc_fixture import write_disc
from animation_retarget_fixture import retarget_fixture


def words(values):
    return b"".join(struct.pack(">I", n & 0xffffffff) for n in values)


class World:
    def __init__(self, seed):
        self.data = bytearray()
        self.rng = random.Random(seed)
        self.root = self.start(0x80000001)

    def start(self, kind):
        exponent = self.rng.randrange(6)
        at = len(self.data)
        self.data.extend(words((kind | exponent << 24, 0)))
        self.data.extend(b"\xa5" * (-len(self.data) % (1 << exponent)))
        return at

    def end(self, at):
        struct.pack_into(">I", self.data, at + 4, len(self.data) - at - 8)
        self.data.extend(b"\x5a" * (-len(self.data) % 4))

    def chunk(self, kind, payload):
        at = self.start(kind)
        self.data.extend(payload)
        self.end(at)

    def hierarchy(self, identity, parent_error=False, name="rig"):
        at = self.start(0x80018000)
        header = [0xf0abcdef] * 13
        header[1], header[2], header[9], header[10] = identity, 2, -1, -1
        encoded_name = name.encode() + b"\0"
        encoded_name += bytes(-len(encoded_name) % 4)
        for kind, payload in (
            (0x18001, words(header)), (0x18002, encoded_name),
            (0x18003, words((0x7ffffff0, 0x80000001))),
            (0x18009, words((-1, 1 if parent_error else 0))),
            (0x18004, words((1, 0))), (0x18005, words((0xfefefefe, 0xdddddddd))),
            (0x18006, words((0xbabababa, 0xbebebebe))), (0x18007, words((1,))),
            (0x18008, words((1, 0))), (0x18010, words((0, 0, 0, 0x3f800000, 0, 0))),
            (0x18011, b"\0\xff"),
        ):
            self.chunk(kind, payload)
        self.end(at)

    def animation(self, identity, signature, value, nodes=2):
        at = self.start(0x80017000)
        header = [0xfedcba98] * 22
        header[1:5] = [identity, 2, nodes, 0]
        header[13], header[21] = 0, signature
        self.chunk(0x17001, words(header))
        self.chunk(0x17002, b"key\0")
        self.chunk(0x17110, words([1] * nodes))
        self.chunk(0x17113, words([0] * nodes))
        for kind in (0x17004, 0x17005, 0x17006, 0x17111, 0x17114):
            self.chunk(kind, words([0xfedcba98] * nodes))
        self.chunk(0x17007, b"")
        self.chunk(0x17008, b"")
        for node in range(nodes):
            n = self.start(0x80017100)
            self.chunk(0x17101, bytes((value, node, 0, 127)))
            self.chunk(0x17112, bytes((value,)))
            self.end(n)
        for kind in (0x17009, 0x1700a, 0x1700b):
            self.chunk(kind, b"")
        self.chunk(0x17003, words([14] * nodes))
        self.end(at)

    def finish(self):
        # An ordinary opaque world record makes whole-file sizes multiples32;
        # the queue exhaustion test then reserves exactly one read per file.
        at = len(self.data)
        count = (-at - 8) % 32
        self.data.extend(words((0x29999, count)) + bytes(count))
        self.end(self.root)
        return bytes(self.data)


def world_pair(seed, mode="valid"):
    a, b = World(seed), World(seed + 91)
    if mode == "orphan":
        a.animation(0x10, 0x1111, 1)
    a.hierarchy(0xaaaa, mode == "parent")
    if mode != "empty":
        a.animation(0x10, 0x1111, seed % 32 + 4)
        a.animation(0x10, 0x9999 if mode == "signature" else 0x1111, seed % 32 + 8,
                    1 if mode == "nodes" else 3 if mode == "extra-nodes" else 2)
    if mode == "retarget":
        a.chunk(0x80017104, words((0, 0, 0, 0)))
    a.hierarchy(0xbbbb)
    a.animation(0x20, 0x2222, seed % 32 + 12)
    b.animation(0x21, 0x2222, seed % 32 + 16)
    if mode == "duplicate":
        b.hierarchy(0xaaaa)
    first, second = a.finish(), b.finish()
    if mode == "root":
        first = b"\x80\0\0\x02" + first[4:]
    if mode == "truncated":
        first = first[:-1]
    if mode == "trailing":
        first += bytes(4)
    return first, second


def compressed(data):
    return words((len(data),)) + zlib.compress(data)


def character_files(name="mario", mode="valid"):
    identity = 0xffffffff
    for byte in name.encode():
        identity = (identity * 33 + byte) & 0xffffffff
    hierarchy = World(15)
    hierarchy.data.clear()
    hierarchy.hierarchy(identity, name="wrong" if mode == "identity" else name)
    animations = World(17)
    animations.data.clear()
    animations.animation(0x10, 0x1111, 8, nodes=3)
    animations.animation(0x20, 0x9999 if mode == "missing-map" else 0x2222, 16, nodes=1)
    maps = [(0x1111, 0, [-1, 3 if mode == "source-node" else 2]), (0x2222, 1, [0, -1])]
    if mode == "map-count":
        maps[0] = (0x1111, 0, [0])
    if mode == "empty-list":
        maps = []
    return {f"art/animation/{name}.shier": bytes(hierarchy.data),
            f"art/animation/{name}fe.sanim": bytes(animations.data),
            f"art/animation/{name}/animretarget/{name}.bin": retarget_fixture(6, [maps])}


def main():
    executable = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="mscharged-animation-bundle-") as folder:
        root = Path(folder)
        for seed in range(36):
            a, b = world_pair(seed)
            (root / f"alignment-{seed}.res").write_bytes(a)
            (root / f"alignment-{seed}.tmp").write_bytes(b)
        for mode in ("orphan", "parent", "empty", "signature", "nodes", "extra-nodes", "retarget",
                     "duplicate", "root", "truncated", "trailing"):
            a, b = world_pair(7, mode)
            (root / f"{mode}.res").write_bytes(a)
            (root / f"{mode}.tmp").write_bytes(b)
        a, b = world_pair(7)
        x, y = world_pair(8)
        bad, _ = world_pair(7, "signature")
        files = {"world.res": a, "world.tmp": b, "other.res": x, "other.tmp": y,
                 "bad.res": bad, "empty.res": b"", "compressed.res.zlib": compressed(a),
                 "compressed.tmp.zlib": compressed(b), "broken.res.zlib": compressed(a)[:-1],
                 "oversize.res.zlib": words((0x1000001,)) + zlib.compress(a)}
        files.update(character_files())
        files.update(character_files("luigi"))
        write_disc(root / "animation.iso", files=files, fst_capacity=0x400)
        result = subprocess.run([executable, str(root / "animation.iso"), str(root), "synthetic"], timeout=45)
        if result.returncode:
            return result.returncode
        for mode in ("identity", "missing-map", "source-node", "map-count", "empty-list", "missing-0", "missing-1", "missing-2"):
            malformed = dict(files)
            malformed.update(character_files(mode=mode))
            if mode in ("missing-0", "missing-1", "missing-2"):
                del malformed[list(character_files())[int(mode[-1])]]
            write_disc(root / "malformed.iso", files=malformed, fst_capacity=0x400)
            result = subprocess.run([executable, str(root / "malformed.iso"), str(root), "bad-character"], timeout=15)
            if result.returncode:
                return result.returncode
        return 0


if __name__ == "__main__":
    sys.exit(main())
