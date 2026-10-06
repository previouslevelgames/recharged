#!/usr/bin/env python3
"""Literal font descriptors and authored row equations; no source layout loop."""
from pathlib import Path
import hashlib
import json
import re
import struct
import sys

from disc_fixture import write_disc


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def signed16(value):
    value &= 0xffff
    return value - 0x10000 if value & 0x8000 else value


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


def code(token):
    return ord(token) if len(token) == 1 else int(token)


def descriptor_facts(data):
    glyphs = {}
    pairs = {}
    for line in data.decode('ascii').splitlines():
        if line.startswith('Height '):
            height, _, ascent, _, leading = map(int, line.split()[1::2])
        elif line.startswith('CharSpacing '):
            spacing = f32(f32(int(line.split()[1])) / 100)
        elif line.startswith('Glyph '):
            match = re.fullmatch(r'Glyph (.+) Width (\d+) (\d+) (-?\d+)(?: RenderHeight (\d+) RenderAscent (\d+) Pos (\d+) (\d+))?', line)
            if not match:
                raise ValueError(line)
            token, advance, _, offset, *_ = match.groups()
            glyphs[code(token)] = (int(advance) & 0xff, signed16(int(offset)) if -128 <= int(offset) <= 127 else ((int(offset) + 128) % 256 - 128))
        elif line.startswith('Kern '):
            fields = line[5:].split(' ')
            for target, value in zip(fields[1::2], fields[2::2]):
                pairs[code(fields[0]), code(target)] = int(value)
    extended = sorted(c for c in glyphs if c >= 0x7f)

    def encoded(ch):
        return ch if ch <= 0x7f else (0x80 + extended.index(ch) if ch in extended else ord('?'))

    def glyph(ch):
        if ch > 0x7f:
            return glyphs[extended[ch - 0x80]]
        return glyphs.get(ch, glyphs[ord('?')])

    def width(ch, previous=0):
        advance, offset = glyph(ch)
        # Descriptor keys remain the original authored keys. FontCharString
        # maps extended input; no convenience Unicode kerning remap is added.
        raw = (advance + offset + pairs.get((previous, ch), 0)) & 0xffffffff
        result = int(f32(f32(raw) * spacing))
        if not 0 <= result <= 0xffffffff:
            raise ValueError('Fixture reaches unqualified font float-to-Wii32 boundary')
        return result

    return dict(height=height, ascent=ascent, leading=leading, spacing=spacing,
                glyphs=glyphs, pairs=pairs, encoded=encoded, width=width)


def sample(facts, tag, text, box, flags, starts, widths, matrix=False):
    count = len(widths)
    assert len(starts) == count + 1 and count <= 16
    assert starts[-1] == len(text)
    x, y = map(f32, box)
    offsets = []
    for width in widths:
        remaining = int(f32(x - f32(width))) if flags & 3 else 0
        offsets.append(signed16(remaining >> (1 if flags & 1 else 0)))
    height = count * facts['height']
    if not flags & 0x30 or f32(height) > y:
        y_offset = 0
    elif flags & 0x10:
        y_offset = signed16(int(f32(y / 2)) - (height >> 1))
    else:
        y_offset = signed16(int(y) - height)
    return dict(tag=tag, text=[ord(c) for c in text], box_bits=[bits(x), bits(y)],
                flags=flags, matrix=matrix, count=count, y_offset=y_offset,
                starts=[n & 0xffff for n in starts], offsets=offsets,
                authored_widths=widths)


def samples(data, wide=False, long_index=False):
    facts = descriptor_facts(data)
    w = facts['width']
    a, aa, b, ba, space = w(ord('A')), w(ord('A'), ord('A')), w(ord('B')), w(ord('B'), ord('A')), w(ord(' '), ord('A'))
    out = []
    if wide:
        assert a == aa == 0x80000000
        for n in (2, 3, 4):
            # An actual overflowing Wii32 sum, rather than a copied wrap loop.
            out.append(sample(facts, f'wii32-width-sum-{n}', 'A' * n,
                              (3000000000, 80), 0, [0, n], [(n * a) & 0xffffffff]))
        return facts, out

    for align in (0, 1, 2, 3):
        for vertical in (0, 0x10, 0x20, 0x30):
            out.append(sample(facts, f'empty-{align}-{vertical}', '',
                              (81.75, 83.75), align | vertical, [0, 0], [0]))
            out.append(sample(facts, f'one-row-{align}-{vertical}', 'AB',
                              (a + ba + 40.75, 83.75), align | vertical,
                              [0, 2], [a + ba], bool(align & 1)))
    for tag, text, width in (
        ('colour-keeps-kerning', 'A{clr:ff0011}B', a + ba),
        ('colour-first-char', '{clr:ff0011}A', a),
        ('unknown-escape', 'A{four}B', a + ba),
        ('openbrace-original-zero-measure', 'A{{}B', a + ba),
        ('nonbreaking-space-original-previous-char', 'A{nbs}B', a + space + ba),
        ('trailing-space', 'A ', a + space),
    ):
        out.append(sample(facts, tag, text, (width + 30.5, 80), 2,
                          [0, len(text)], [width], True))
    translated = facts['encoded'](0xe9)
    ext_width = w(translated) + w(ord('B'), translated)
    out.append(sample(facts, 'original-extended-or-fallback', '\u00e9B',
                      (ext_width + 40.5, 80), 1, [0, 2], [ext_width]))
    out.append(sample(facts, 'paragraph-preserves-previous-char', 'A{p}B',
                      (max(a, ba) + 40.5, 80), 2 | 0x10,
                      [0, 4, 5], [a, ba], True))
    out.append(sample(facts, 'paragraph-no-wrap-source-reset', 'AB{p}A',
                      (max(a, b, ba) + 40.5, 80), 0x1000 | 2,
                      [0, 6], [w(ord('A'), ord('B'))]))
    out.append(sample(facts, 'signed-short-x-narrowing', 'AB',
                      (40000.75, 80), 2, [0, 2], [a + ba]))
    out.append(sample(facts, 'signed-short-y-narrowing', 'AB',
                      (a + ba + 40, 90000.75), 0x20, [0, 2], [a + ba]))
    out.append(sample(facts, 'height-overflow-source-zero-offset', 'AB',
                      (a + ba + 40, max(0, facts['height'] - 0.25)), 0x30,
                      [0, 2], [a + ba]))
    out.append(sample(facts, 'draw-only-flags-no-layout-change', 'AB',
                      (a + ba + 40, 80), 0x100 | 0x200 | 0x800 | 0x10,
                      [0, 2], [a + ba]))
    # These explicit row boundaries exercise hard/space wrapping. No native
    # source, helper or equivalent wrapping traversal is imported here.
    if aa and a + aa < 1000000:
        out.append(sample(facts, 'hard-wrap-retry-previous-char', 'AAA',
                          (a + aa, 80), 2 | 0x400, [0, 2, 3], [a + aa, aa]))
    if aa and space and a + space >= aa:
        out.append(sample(facts, 'space-wrap-excludes-break-space', 'A A',
                          (a + space, 80), 2, [0, 2, 3], [a, aa]))
        out.append(sample(facts, 'dont-wrap-on-spaces-keeps-width', 'A A',
                          (a + space, 80), 2 | 0x400, [0, 2, 3], [a + space, aa]))
    text = 'A{p}' * 15 + 'A'
    out.append(sample(facts, 'original-sixteen-rows-and-sentinel', text,
                      (max(a, aa) + 40, 600), 1 | 0x10,
                      [4 * i for i in range(16)] + [len(text)], [a] + [aa] * 15))
    if long_index:
        text = 'A' * 65537
        out.append(sample(facts, 'original-u16-end-index-narrowing', text,
                          (1000, 80), 0x1000, [0, len(text)], [(a + aa * 65536) & 0xffffffff]))
    return facts, out


def bundle(base, descriptor):
    raw = bytearray(64)
    struct.pack_into('>4I', raw, 0, 32, 1, 1, 2)
    struct.pack_into('>3I', raw, 32, name_hash(base), 2, len(descriptor))
    return bytes(raw) + descriptor


def owned_descriptor(raw, base):
    sector, count, directory, _ = struct.unpack_from('>4I', raw)
    entries = [struct.unpack_from('>3I', raw, sector * directory + 12 * i)
               for i in range(count)]
    matches = [(block, size) for key, block, size in entries if key == name_hash(base)]
    assert len(matches) == 1
    block, size = matches[0]
    assert block * sector + size <= len(raw)
    return raw[block * sector:block * sector + size]


def quote(value):
    return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"') + '"'


def generate(output, owned=None, owned_disc=None):
    output.mkdir(exist_ok=False)
    descriptions = {
        'packed-kern': ('NLG Font Description file\r\nVersion 1.1\r\nPageSize 64 PageCount 1 TexType color Distribution english\r\n'
            'Height 12 RenderHeight 16 Ascent 9 RenderAscent 11 IL 1\r\nCharSpacing 125 LineHeight 200\r\n'
            'Glyph ? Width 8 8 0\r\nGlyph A Width 10 8 1\r\nGlyph B Width 12 8 -1\r\nGlyph 32 Width 4 0 0\r\n'
            'Glyph 233 Width 12 8 -1\r\nKern A B -2 32 1\r\nKern B A -1\r\nEND'),
        'authored-12': ('NLG Font Description file\r\nVersion 1.2\r\nPageSize 64 PageCount 1 TexType greyscale Distribution english\r\n'
            'Height 16 RenderHeight 20 Ascent 11 RenderAscent 14 IL 2\r\nCharSpacing 100 LineHeight 125\r\n'
            'Glyph ? Width 10 8 0 RenderHeight 15 RenderAscent 12 Pos 4 9\r\n'
            'Glyph A Width 12 10 2 RenderHeight 18 RenderAscent 14 Pos 22 13\r\n'
            'Glyph B Width 12 11 -1 RenderHeight 19 RenderAscent 15 Pos 37 17\r\n'
            'Glyph 32 Width 4 0 0 RenderHeight 0 RenderAscent 0 Pos 0 0\r\n'
            'Glyph 233 Width 12 11 0 RenderHeight 18 RenderAscent 15 Pos 18 44\r\nEND'),
        'wide-word': ('NLG Font Description file\r\nVersion 1.1\r\nPageSize 64 PageCount 1 TexType color Distribution english\r\n'
            'Height 12 RenderHeight 16 Ascent 9 RenderAscent 11 IL 1\r\nCharSpacing 1677721600 LineHeight 100\r\n'
            'Glyph ? Width 8 8 0\r\nGlyph A Width 128 8 0\r\nGlyph B Width 128 8 0\r\nGlyph 32 Width 4 0 0\r\nEND'),
    }
    records = []

    def record(stem, raw, data, private):
        index = len(records)
        base = 'fe/fonts/' + stem
        facts, cases = samples(data, wide=stem == 'wide-word', long_index=stem == 'packed-kern')
        image = Path(owned_disc).resolve() if private and owned_disc else output / f'{index}.iso'
        path = '/Art/fe/fonts/' + stem + '.res'
        if not (private and owned_disc):
            # Optional private descriptor-only fixtures retain exact descriptor
            # bytes in a literal minimal bundle. Actual owned-disc qualification
            # instead reads the complete original bundle through real NL/DVD.
            fixture_bundle = bundle(base, data) if private else raw
            size = (0x3200 + len(fixture_bundle) + 0x7fff) & ~0x7fff
            write_disc(image, files={path.lstrip('/'): fixture_bundle}, partition_size=max(size, 0x10000))
        oracle = output / f'{index}.oracle'
        lines = [f"{len(data)} {data_hash(data)} {facts['height']} {facts['ascent']} {facts['leading']} {bits(facts['spacing'])} {len(cases)}"]
        for case in cases:
            lines.append(' '.join(map(str, [case['tag'], *case['box_bits'], case['flags'], int(case['matrix']),
                                            case['count'], case['y_offset'], len(case['text']), *case['text']])))
            lines.append(' '.join(map(str, case['starts'])))
            lines.append(' '.join(map(str, case['offsets'])))
        oracle.write_text('\n'.join(lines) + '\n')
        records.append(dict(stem=stem, disc=str(image), bundle=path, descriptor=base,
                            oracle=str(oracle), descriptor_sha256=hashlib.sha256(data).hexdigest(),
                            bundle_sha256=hashlib.sha256(raw).hexdigest(), private=private,
                            actual_owned_disc=bool(private and owned_disc),
                            samples=len(cases), authored_equations=cases))

    for stem, text in descriptions.items():
        data = text.encode('ascii')
        record(stem, bundle('fe/fonts/' + stem, data), data, False)
    if owned:
        for path in sorted(Path(owned).glob('*.res')):
            raw = path.read_bytes()
            record(path.stem, raw, owned_descriptor(raw, 'fe/fonts/' + path.stem), True)
    manifest = [str(len(records))]
    for row in records:
        manifest.append(' '.join(quote(row[key]) for key in ('disc', 'bundle', 'descriptor', 'oracle')))
    (output / 'manifest.txt').write_text('\n'.join(manifest) + '\n')
    (output / 'manifest.json').write_text(json.dumps(dict(
        scope='Explicit authored row boundaries and independent descriptor width/float/narrowing equations. No wrapping algorithm or game layout helper is imported.',
        records=records), indent=2) + '\n')
    print('Original text layout fonts:', len(records), 'samples:', sum(row['samples'] for row in records))


if __name__ == '__main__':
    generate(Path(sys.argv[1]), Path(sys.argv[2]) if len(sys.argv) > 2 else None,
             Path(sys.argv[3]) if len(sys.argv) > 3 else None)
