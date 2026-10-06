#!/usr/bin/env python3
"""Independent literal Wii LOC data and full scalar/code-unit oracles."""
from pathlib import Path
import hashlib
import importlib.util
import json
import struct
import sys

W = Path(__file__).resolve().parent
ROOT = W.parent
OUT = Path(sys.argv[1])
OUT.mkdir(exist_ok=False)
OWNED = Path(sys.argv[2]) if len(sys.argv) > 2 else None
spec = importlib.util.spec_from_file_location('synthetic_disc', ROOT / 'tests/disc_fixture.py')
disc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(disc)
NAMES = ['English', 'French', 'German', 'Spanish', 'Italian', 'Japanese',
         'UKEnglish', 'NAFrench', 'NASpanish', 'Longest', 'Bob']
IDS = [0x7A947B29, 0xA93C2035, 0xAAAD26B9, 0xB482A4B5, 0xBC0FCCA1,
       0x95F1D726, 0x5F2F5E69, 0x30D469C4, 0x2F242024, 0x983D29BB, 0x00012332]


def hash_name(text):
    value = 0xFFFFFFFF
    for ch in text.lower().encode('ascii'):
        value = (value * 33 + ch) & 0xFFFFFFFF
    return value


def units(text):
    data = text.encode('utf-16-be')
    return list(struct.unpack('>' + str(len(data) // 2) + 'H', data)) + [0]


def encode(language, game):
    pool = []
    entries = []
    for query, value in [('alpha', f'Language {language}/{int(game)}'),
                         ('emoji', 'A\U0001F642\u03A9'),
                         ('omega', '\u65E5\u672C\u8A9E'),
                         ('credits_copyright', 'Copyright')]:
        entries.append((hash_name(query), len(pool)))
        pool.extend(units(value))
    entries.append((hash_name('alias'), entries[0][1]))
    pool.extend([0xDEAD, 0xBEEF, 0])  # Unreferenced authored code units retained.
    entries.sort()
    raw = struct.pack('>4s4I', b'NLOC', 1, IDS[language], len(entries), 0xF1E2D3C4)
    raw += b''.join(struct.pack('>II', *entry) for entry in entries)
    raw += struct.pack('>' + str(len(pool)) + 'H', *pool)
    return raw


records = []


def record(raw, filename, image, language, game, accepted, tag):
    index = len(records)
    raw_path = OUT / f'{index}.loc'
    raw_path.write_bytes(raw)
    magic, version, ident, count, flags = struct.unpack('>4s4I', raw[:20])
    queries = []
    oracle = OUT / f'{index}.oracle'
    if accepted:
        first_string = 20 + 8 * count
        entries = [struct.unpack_from('>II', raw, 20 + 8 * i) for i in range(count)]
        pool = list(struct.unpack('>' + str((len(raw) - first_string) // 2) + 'H', raw[first_string:]))
        expected = struct.pack('<4s4I', magic, version, ident, count, flags)
        expected += b''.join(struct.pack('<II', *entry) for entry in entries)
        expected += struct.pack('<' + str(len(pool)) + 'H', *pool)
        oracle.write_bytes(expected)
        available = dict(entries)
        for query in ['alpha', 'ALPHA', 'alias', 'emoji', 'omega', 'credits_copyright', 'ns', 'NT', 'missing_query']:
            key = hash_name(query)
            offset = available.get(key)
            found = []
            if offset is not None:
                at = offset
                while pool[at] != 0:
                    found.append(pool[at]); at += 1
                found.append(0)
            queries.append({'query': query, 'found': offset is not None, 'units': found})
    records.append({'index': index, 'image': str(image), 'filename': filename,
                    'language': language, 'game': bool(game), 'accepted': bool(accepted),
                    'tag': tag, 'raw': str(raw_path), 'raw_sha256': hashlib.sha256(raw).hexdigest(),
                    'size': len(raw), 'header': [version, ident, count, flags],
                    'oracle': str(oracle) if accepted else None, 'queries': queries})


generated = OUT / 'generated.iso'
files = {}
for language, name in enumerate(NAMES):
    for game in [False, True]:
        filename = 'art/fe/' + name + ('_game' if game else '') + '.loc'
        raw = encode(language, game)
        files[filename] = raw
        record(raw, filename, generated, language, game, True, 'generated')
disc.write_disc(generated, files=files, fst_capacity=0x800, partition_size=0x10000)
for which in range(3):
    # Invalid accepted-domain geometry must not be interpreted after the
    # original callback rejects its header. Huge count deliberately tests that.
    raw = struct.pack('>4s4I', b'BAD!' if which == 0 else b'NLOC',
                      2 if which == 1 else 1,
                      IDS[1] if which == 2 else IDS[0], 0xFFFFFFFF, 0x13579BDF)
    image = OUT / f'reject{which}.iso'
    disc.write_disc(image, files={'art/fe/English.loc': raw})
    record(raw, 'art/fe/English.loc', image, 0, False, False, 'original-header-rejection')
if OWNED:
    for language, name in enumerate(NAMES):
        for game in [False, True]:
            filename = 'art/fe/' + name + ('_game' if game else '') + '.loc'
            raw = (OWNED / (name.lower() + ('_game' if game else '') + '.loc')).read_bytes()
            assert struct.unpack('>4sII', raw[:12]) == (b'NLOC', 1, IDS[language])
            image = OUT / f'owned{language}-{int(game)}.iso'
            size = (0x3200 + len(raw) + 0x7FFF) & ~0x7FFF
            disc.write_disc(image, files={filename: raw}, partition_size=size)
            record(raw, filename, image, language, game, True, 'owned-private')

def quote_path(value):
    return chr(34) + str(value).replace(chr(92), chr(92) * 2).replace(chr(34), chr(92) + chr(34)) + chr(34)


manifest = [str(len(records))]
for row in records:
    manifest.append(f"{quote_path(row['image'])} {row['language']} {int(row['game'])} {int(row['accepted'])} {row['size']} {quote_path(row['oracle'] or '-')} {len(row['queries'])}")
    for q in row['queries']:
        manifest.append(' '.join(map(str, [q['query'], int(q['found']), len(q['units']), *q['units']])))
(OUT / 'manifest.txt').write_text('\n'.join(manifest) + '\n')
(OUT / 'manifest.json').write_text(json.dumps({'encoder': 'Independent literal 20/8-byte big-endian fields and UTF16 units. No native transport or game manager imported.',
    'cases': records, 'private': bool(OWNED), 'scope': 'Exact actual source language filenames, both branches, original invalid-header decisions; all raw/header/table/code-unit data compared independently.'}, indent=2) + '\n')
print('Independent LOC cases:', len(records), 'owned:', sum(r['tag'] == 'owned-private' for r in records))
