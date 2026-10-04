#!/usr/bin/env python3
"""Synthetic Wii effects records; no game assets are redistributed."""
import pathlib
import struct
import subprocess
import sys
import tempfile
from effects_registry_fixture import chunk, words, textures


def resident(mode='basic'):
    data = bytearray(0x78 + 25 * 4)
    struct.pack_into('>If', data, 0, 0x1234, 2)
    for at, value in ((8, 1), (0x10, 1), (0x4c, 4)):
        struct.pack_into('>f', data, at, value)
    data[0x34] = {'sphere': 1, 'spindle': 2, 'hemisphere': 3, 'disc': 4}.get(mode, 0)
    data[0x37] = 2 if mode == 'local' else 0
    struct.pack_into('>II', data, 0x38, 0x12345678, 4)
    struct.pack_into('>I', data, 0x54, 0xffffffff)
    for i in range(25):
        data[0x78+i*4:0x7c+i*4] = bytes((i*8, i*4, i*2, 255-i*8))
    if mode == 'model': struct.pack_into('>I', data, 0x54, 7)
    if mode == 'light': data[0x37] = 4
    if mode == 'event': struct.pack_into('>I', data, 0x40, 1)
    if mode == 'frames': struct.pack_into('>I', data, 0x3c, 3)
    if mode == 'life': struct.pack_into('>f', data, 0x10, 0)
    if mode == 'missing': struct.pack_into('>I', data, 0x38, 123)
    if mode == 'burst': struct.pack_into('>f', data, 4, 0)
    if mode == 'angle':
        struct.pack_into('>f', data, 0x28, 10000)
        struct.pack_into('>f', data, 4, 0)
        struct.pack_into('>f', data, 0x10, 4)
    if mode == 'flip': struct.pack_into('>f', data, 0x30, 100)
    if mode.startswith('atlas'): struct.pack_into('>I', data, 0x3c, int(mode[5:]))
    body = chunk(0x24003, data)
    for i, value in enumerate((8, 2, 1, 0, 0, 2, 0, 0)):
        if mode == 'angle' and i == 3: value = 10000
        if mode == 'curve' and i in (0, 1, 3, 5):
            coefficients = {0: (0, 0, 0, 8), 1: (0, 0, 2, 2), 3: (0, 0, 0, 0), 5: (0, 0, 0, 2)}[i]
            prop = words(1, 0x7fc00000, 0x7fc00000, 2, 0xfeedface)
            tail = (0, 0, 2, 4) if i == 1 else coefficients
            keys = struct.pack('>10f', 0, *coefficients, .5, *tail)
            body += chunk(0x80024004, chunk(0x24005, prop) + chunk(0x24006, keys))
        else:
            prop = struct.pack('>IffII', 0, value, 0, 0xdeadbeef, 0xfeedface)
            body += chunk(0x80024004, chunk(0x24005, prop))
    template = chunk(0x80024002, body)
    spec = bytearray(88)
    struct.pack_into('>I', spec, 0, 0x5678)
    struct.pack_into('>ff', spec, 0x3c, -1, -1)
    struct.pack_into('>i', spec, 0x48, 0)
    if mode == 'delay': struct.pack_into('>f', spec, 0x10, .375)
    if mode == 'linger': struct.pack_into('>ff', spec, 0x3c, .25, .5)
    if mode == 'badlinger': struct.pack_into('>ff', spec, 0x3c, -.25, .5)
    if mode == 'pose': struct.pack_into('>I', spec, 8, 1)
    group = chunk(0x80024020, chunk(0x24021, words(0x81f2a311, 0, 1, 0, 0, 0, 0))
                  + chunk(0x24022, spec) + chunk(0x24023, b''))
    entry = chunk(0x24001, words(0, 0, 1, 0, 1, 0))
    entry += chunk(0x24025, words(0)) + chunk(0x24026, words(0)) + template + group
    return chunk(0x80000001, chunk(0x80024000, entry))


def run(executable):
    with tempfile.TemporaryDirectory(prefix='charged-particle-') as temporary:
        folder = pathlib.Path(temporary)
        modes = ('basic', 'local', 'sphere', 'spindle', 'hemisphere', 'disc', 'curve', 'burst', 'delay', 'linger',
                 'badlinger', 'model', 'light', 'event', 'frames', 'life', 'missing', 'pose', 'angle', 'flip',
                 'atlas1', 'atlas9', 'atlas16', 'atlas25', 'atlas36')
        for mode in modes: (folder / (mode + '.bun')).write_bytes(resident(mode))
        (folder / 'nonresident.bun').write_bytes(chunk(0x80000001, chunk(0x24100, textures())))
        (folder / 'geometry.bun').write_bytes(b'opaque model data stays unselected')
        (folder / 'textures.rlt').write_bytes(textures(0x87654321))
        subprocess.run([executable, str(folder)], check=True)

if __name__ == '__main__': run(sys.argv[1])
