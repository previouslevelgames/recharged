#!/usr/bin/env python3
"""Independent Wii bundle/page byte oracles for actual font callbacks."""
from pathlib import Path
import hashlib
import json
import re
import struct
import sys
from disc_fixture import write_disc


def name_hash(name):
    value = 0xffffffff
    for ch in name.lower().encode('ascii'):
        value = (value * 33 + ch) & 0xffffffff
    return value


def data_hash(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return value


def bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def bundle(entries):
    sector = 32
    first = (32 + len(entries) * 12 + 31) // 32
    raw = bytearray(first * sector)
    struct.pack_into('>4I', raw, 0, sector, len(entries), 1, first)
    for index, (name, payload) in enumerate(entries):
        raw.extend(b'\0' * (-len(raw) % sector))
        block = len(raw) // sector
        struct.pack_into('>3I', raw, 32 + index * 12, name_hash(name), block, len(payload))
        raw.extend(payload)
    return bytes(raw)


def payloads(raw):
    sector, count, directory, _ = struct.unpack_from('>4I', raw)
    result = {}
    for index in range(count):
        key, block, size = struct.unpack_from('>3I', raw, sector * directory + 12 * index)
        start = block * sector
        assert start + size <= len(raw)
        assert key not in result
        result[key] = raw[start:start + size]
    return result


def texture_bytes(fmt, width, height, levels):
    # Independent GX tile geometry, with each level padded to a complete tile.
    tx, ty, stride = ((4, 4, 32), (4, 4, 32), (8, 8, 32), (4, 4, 64),
                      (8, 4, 32), (8, 8, 32), (8, 4, 32), (4, 4, 32),
                      (8, 4, 32))[fmt]
    length = 0
    for _ in range(levels):
        length += ((width + tx - 1) // tx) * ((height + ty - 1) // ty) * stride
        width, height = max(1, width // 2), max(1, height // 2)
    return length


def texture(fmt, seed, width=32, height=32, levels=1):
    palette = 16 if fmt == 8 else 0
    header = bytearray(32)
    struct.pack_into('>2I', header, 0, levels, fmt)
    header[8:12] = bytes((8, 8, 8, 8))
    header[12] = seed & 1
    struct.pack_into('>2H', header, 14, width, height)
    struct.pack_into('>I', header, 20, palette)
    return bytes(header) + bytes(((i * 19 + seed) & 255)
                                 for i in range(texture_bytes(fmt, width, height, levels) + palette * 2))


def descriptor(pages, typ, height):
    return (f'NLG Font Description file\r\nVersion 1.2\r\n'
            f'PageSize 32 PageCount {pages} TexType {typ} Distribution english\r\n'
            f'Height {height} RenderHeight 16 Ascent 9 RenderAscent 11 IL 2\r\n'
            f'CharSpacing 125 LineHeight 150\r\n'
            'Glyph ? Width 8 8 0 RenderHeight 12 RenderAscent 10 Pos 1 2\r\n'
            'Glyph A Width 10 8 1 RenderHeight 12 RenderAscent 10 Pos 10 2\r\n'
            'Glyph B Width 12 8 -1 RenderHeight 12 RenderAscent 10 Pos 20 2\r\n'
            'Glyph 233 Width 12 8 -1 RenderHeight 12 RenderAscent 10 Pos 1 18\r\n'
            'Kern A B -2\r\nEND').encode('ascii')


def facts(base, alias, raw):
    entries = payloads(raw)
    text = entries[name_hash(base)]
    lines = text.decode('ascii').splitlines()
    page = next(line for line in lines if line.startswith('PageSize ')).split()
    pages, typ = int(page[3]), {'color': 1, 'colour': 1, 'greyscale': 2, 'splitfx': 3,
                              'split': 3}[page[5].lower()]
    height = next(line for line in lines if line.startswith('Height ')).split()
    spacing = next(line for line in lines if line.startswith('CharSpacing ')).split()
    f32 = lambda v: struct.unpack('<f', struct.pack('<f', v))[0]
    result = dict(descriptor_size=len(text), descriptor_hash=data_hash(text),
                  alias_hash=name_hash(alias), pages=pages, texture_type=typ,
                  height=int(height[1]), ascent=int(height[5]), leading=int(height[9]),
                  spacing=bits(f32(f32(int(spacing[1])) / 100)), textures=[])
    for page_index in range(pages):
        for effect in range(2 if typ == 3 else 1):
            name = base + '_' + str(page_index + 1) + ('e' if effect else '')
            key = name_hash(name)
            texture = entries[key]
            levels, fmt = struct.unpack_from('>2I', texture)
            width, height = struct.unpack_from('>2H', texture, 14)
            palette = struct.unpack_from('>I', texture, 20)[0]
            size = texture_bytes(fmt, width, height, levels)
            assert 32 + size + palette * 2 <= len(texture)
            result['textures'].append(dict(name=name, hash=key, page=page_index, effect=effect,
                width=width, height=height, levels=levels, format=fmt,
                data_bytes=size, data_hash=data_hash(texture[32:32 + size]),
                palette_count=palette, palette_hash=data_hash(texture[32 + size:32 + size + palette * 2]),
                bits=int.from_bytes(texture[8:12], 'big'), missing=int(bool(texture[12])),
                raw_size=len(texture), raw_sha256=hashlib.sha256(texture).hexdigest()))
    return result


def quote(value):
    return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"') + '"'


def generate(output, owned=None, owned_disc=None):
    output.mkdir(exist_ok=False)
    records, files = [], {}
    # Literal manager calls in a diagnostic exercise allocation/callback limits.
    # This does not change, duplicate or claim execution of FontLoading branches.
    profiles = [('colour', 1, 'color', 3), ('grey', 2, 'greyscale', 4),
                ('split', 8, 'splitfx', 4), ('indexed', 1, 'color', 8)]
    # The original GCTextureSize handles enum5 through its two-byte fallback.
    # Do not invent an I4 size fix or feed malformed undersized source input.
    source_profiles = (0, 1, 2, 3, 4, 6, 7, 8)
    for index in range(16):
        profiles.append((f'slot{index}', 1, 'color', source_profiles[index % len(source_profiles)]))
    for index, (stem, pages, typ, fmt) in enumerate(profiles):
        base = 'fe/fonts/fixture163-' + stem
        text = descriptor(pages, typ, 12 + index)
        entries = [(base, text)]
        for page in range(1, pages + 1):
            entries.append((base + '_' + str(page), texture(fmt, 17 * index + page)))
            if typ == 'splitfx':
                entries.append((base + '_' + str(page) + 'e', texture(4, 31 * index + page)))
        raw = bundle(entries)
        path = 'Art/fe/fonts/fixture163-' + stem + '.res'
        files[path] = raw
        records.append(dict(stem=stem, bundle=path, descriptor=base, alias='Case' + stem,
                            raw=raw, private=False, concurrent=stem.startswith('slot')))
    image = output / 'generated.iso'
    total = sum(len(v) for v in files.values())
    write_disc(image, files=files, fst_capacity=0x1000,
               partition_size=max(0x80000, (total + 0x2ffff) & ~0x7fff))
    for row in records:
        row['disc'] = image
    if owned:
        assert owned_disc, 'Optional owned full bundles require the actual supplied disc'
        for path in sorted(Path(owned).glob('*.res')):
            records.append(dict(stem=path.stem, disc=Path(owned_disc).resolve(),
                bundle='Art/fe/fonts/' + path.name, descriptor='fe/fonts/' + path.stem,
                alias='fot-rodinprob18' if 'text' in path.stem else 'Scratchy36',
                raw=path.read_bytes(), private=True, concurrent=False))
    for index, row in enumerate(records):
        raw = row.pop('raw')
        oracle = facts(row['descriptor'], row['alias'], raw)
        path = output / f'{index}.oracle'
        lines = [' '.join(map(str, [oracle[k] for k in (
            'descriptor_size', 'descriptor_hash', 'alias_hash', 'pages', 'texture_type',
            'height', 'ascent', 'leading', 'spacing')] + [len(oracle['textures'])]))]
        keys = ('hash', 'page', 'effect', 'width', 'height', 'levels', 'format',
                'data_bytes', 'data_hash', 'palette_count', 'palette_hash', 'bits', 'missing')
        lines += [' '.join(map(str, [tex[k] for k in keys])) for tex in oracle['textures']]
        path.write_text('\n'.join(lines) + '\n')
        row.update(oracle=str(path), bundle_sha256=hashlib.sha256(raw).hexdigest(), facts=oracle)
        row['disc'] = str(row['disc'])
    manifest = [str(len(records))]
    for row in records:
        manifest.append(' '.join(quote(row[key]) for key in ('disc', 'bundle', 'descriptor', 'alias', 'oracle'))
                        + ' ' + str(int(row['concurrent'])))
    (output / 'manifest.txt').write_text('\n'.join(manifest) + '\n')
    (output / 'manifest.json').write_text(json.dumps(dict(
        scope='Literal descriptor fields, Wii bundle entries and independent tiled GX page/palette byte geometry/hash oracles. Synthetic diagnostic calls and optional all22 original bundles are not alternate gameplay font requests.',
        records=records), indent=2) + '\n')
    print('Original callback fonts:', len(records), 'textures:', sum(len(row['facts']['textures']) for row in records))


if __name__ == '__main__':
    generate(Path(sys.argv[1]), Path(sys.argv[2]) if len(sys.argv) > 2 else None,
             Path(sys.argv[3]) if len(sys.argv) > 3 else None)
