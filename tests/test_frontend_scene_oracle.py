"""Independent FENL raw-offset oracle; owned files are supplied locally, never fixtures.

Usage: python test_frontend_scene_oracle.py READER OWNED_FEN_FOLDER
Fields follow the pinned Wii FEPackage/FEPresentation/TL*/FELibObject layouts.
The oracle reads float words as integers, independently of native float decoding.
"""
import pathlib
import struct
import subprocess
import sys


def fingerprint(file):
    magic, version, length, relocation_length = struct.unpack_from('>4I', file)
    assert magic == 0x46454E4C and version == 1
    assert len(file) == 16 + length + relocation_length
    data = file[16:16 + length]
    offsets = set(struct.unpack(f'>{relocation_length // 4}I', file[16 + length:]))
    u32 = lambda at: struct.unpack_from('>I', data, at)[0]
    u16 = lambda at: struct.unpack_from('>H', data, at)[0]
    null = 0xFFFFFFFF

    def pointer(at):
        if at not in offsets:
            assert u32(at) == 0
            return null
        return u32(at)

    def ring(tail):
        if tail == null:
            return []
        result, current = [], pointer(tail)
        while current not in result:
            result.append(current)
            if current == tail:
                assert pointer(current) == result[0]
                return result
            current = pointer(current)
        raise AssertionError('Invalid oracle ring')

    libraries = ring(pointer(12))
    resources = ring(pointer(8))
    presentation = pointer(4)
    slides, instances = set(), set()

    def visit_instances(tail):
        for at in ring(tail):
            assert at not in instances
            instances.add(at)
            visit_instances(pointer(at + 8))

    def visit_slides(tail):
        for at in ring(tail):
            assert at not in slides
            slides.add(at)
            visit_instances(pointer(at + 8))

    for at in libraries:
        if u32(at + 0x74) == 3:
            visit_slides(pointer(at + 0x78))
    visit_slides(pointer(presentation))
    output = bytearray()
    def word(value):
        output.extend(struct.pack('>I', value))
    def words(values):
        for value in values:
            word(value)
    def name(at):
        value = data[at:at + 32].split(b'\0', 1)[0]
        word(len(value)); output.extend(value)
    def refs(values):
        word(len(values)); words(values)
    def attributes(at):
        words(u32(at + i * 4) for i in range(12))
        word(data[at + 48]); words(data[at + 49:at + 53])
        words(u32(at + 56 + i * 4) for i in range(4))

    words([u32(16), len(offsets), u32(presentation + 8), pointer(presentation + 4)])
    refs(ring(pointer(presentation)))
    word(len(resources))
    for at in sorted(resources):
        words([at, u32(at + 8), u32(at + 12), u32(at + 20), data[at + 16]])
    word(len(libraries))
    for at in sorted(libraries):
        kind = u32(at + 0x74)
        words([at, kind, u32(at + 0x50)]); name(at + 0x54); attributes(at + 8)
        word(pointer(at + 0x78) if kind in (1, 2) else null)
        word(pointer(at + 0x7C) if kind == 3 else null)
        refs(ring(pointer(at + 0x78)) if kind == 3 else [])
        words([u32(at + 0x80), u32(at + 0x84)] if kind == 2 else [0, 0])
        words(data[at + 0x7C:at + 0x80] if kind == 2 else [0] * 4)
    word(len(instances))
    for at in sorted(instances):
        kind = u32(at + 0x88)
        words([at, kind, u32(at + 0x38), u32(at + 0x84), u16(at + 0x8C)])
        name(at + 0x18); words([u32(at + 0x10), u32(at + 0x14), data[at + 0x8E]])
        attributes(at + 0x3C); word(pointer(at + 12))
        word(pointer(at + 0x90) if kind == 2 else null); refs(ring(pointer(at + 8)))
        words([u32(at + 0x90), u32(at + 0xA0), u32(at + 0x100)] if kind == 3 else [0] * 3)
        words([u32(at + 0x98), u32(at + 0x9C)] if kind == 3 else [0] * 2)
        words(data[at + 0x94:at + 0x98] if kind == 3 else [0] * 4)
        text = []
        start = pointer(at + 0x104) if kind == 3 else null
        if start != null:
            while u16(start):
                text.append(u16(start)); start += 2
        refs(text)
    word(len(slides))
    for at in sorted(slides):
        words([at, u32(at + 0x40), u32(at + 0x1C)]); name(at + 0x20)
        words([u32(at + 0x10), u32(at + 0x14), u32(at + 0x18), data[at + 0x44], int(pointer(at + 12) != null)])
        refs(ring(pointer(at + 8)))
    hash_value = 14695981039346656037
    for byte in output:
        hash_value = ((hash_value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f'{hash_value:016x}', (len(slides), len(instances), len(libraries), len(resources))


def main():
    reader, directory = pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]).resolve()
    run = subprocess.run([str(reader), str(directory)], capture_output=True, text=True, timeout=60, check=True)
    decoded = {row[0]: row[1:] for line in run.stdout.splitlines() if len(row := line.split('\t')) == 6}
    for path in sorted(directory.glob('*.fen')):
        expected_hash, counts = fingerprint(path.read_bytes())
        row = decoded.pop(path.name)
        assert tuple(map(int, row[:4])) == counts, path.name
        assert row[4] == expected_hash, (path.name, row[4], expected_hash)
        print(path.name, expected_hash)
    assert not decoded and len(list(directory.glob('*.fen'))) == 75
    print('75 owned FEN graphs match independent raw scalar bits, names and ordered-reference oracle')


if __name__ == '__main__':
    main()
