"""Synthetic model-particle source records; retail assets remain private."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from effects_registry_fixture import chunk, words


def texture(identity):
    data = bytearray(96)
    struct.pack_into('>II', data, 0, 1, 3)
    data[8:12] = bytes((8, 8, 8, 0))
    struct.pack_into('>HH', data, 14, 4, 4)
    for pixel in range(16):
        data[32 + pixel*2:34 + pixel*2] = bytes((255, 200))
        data[64 + pixel*2:66 + pixel*2] = bytes((100, 50))
    return words(0x50544c47, 1, 0, 0, identity, 0, len(data), 0) + data


def resident(mode):
    header = bytearray(0x78 + 25*4)
    struct.pack_into('>If', header, 0, 0x13579bdf, 2)
    struct.pack_into('>f', header, 0x10, 1)
    struct.pack_into('>f', header, 0x28, 30)
    header[0x35] = mode == 'additive'
    header[0x36] = 0 if mode == 'facing' else 1
    header[0x37] = 2 | (8 if mode == 'lifeframe' else 0) | (4 if mode == 'depth' else 0)
    struct.pack_into('>II', header, 0x38, 0x12345678, 1)
    struct.pack_into('>fI', header, 0x4c, 4, 0) # FPS range
    struct.pack_into('>I', header, 0x54, 0x10203040)
    for i in range(25):
        header[0x78+i*4:0x7c+i*4] = bytes((i*8, i*4, i*2, 255))
    if mode == 'gpu':
        struct.pack_into('>f', header, 0x28, 0)
        struct.pack_into('>f', header, 4, 0)
        header[0x78:] = bytes((128, 255, 128, 255))*25
    if mode == 'missing_model': struct.pack_into('>I', header, 0x54, 0x9abc)
    if mode == 'nonanimated': struct.pack_into('>I', header, 0x54, 0xffffffff)
    if mode == 'missing_texture': struct.pack_into('>I', header, 0x38, 0x9abc)
    if mode == 'negative_fps': struct.pack_into('>f', header, 0x4c, -1)
    if mode == 'zero_life': struct.pack_into('>f', header, 0x10, 0)
    body = chunk(0x24003, header)
    for value in (1 if mode == 'gpu' else 8, 2, 1, 0, 0, 0, 0, 0):
        body += chunk(0x80024004, chunk(0x24005, struct.pack('>IffII', 0, value, 0, 0, 0)))
    spec = bytearray(88)
    struct.pack_into('>4I', spec, 0, 1, 0, 0 if mode == 'gpu' else 3, 0x11112222)
    struct.pack_into('>ff', spec, 60, -1, -1)
    struct.pack_into('>I', spec, 72, 0 if mode == 'gpu' else 3)
    if mode == 'ground':
        struct.pack_into('>I', spec, 32, 1)
        struct.pack_into('>f', spec, 40, .25)
    if mode == 'ascend': struct.pack_into('>I', spec, 20, 1)
    if mode == 'light': struct.pack_into('>I', spec, 36, 1)
    if mode == 'terrain': struct.pack_into('>I', spec, 56, 1)
    if mode == 'axis': struct.pack_into('>I', spec, 72, 7)
    if mode == 'layer': struct.pack_into('>I', spec, 68, 0x7fffffff)
    group = chunk(0x80024020, chunk(0x24021, words(0x81f2a311, 0, 1, 0, 0, 0, 0))
                  + chunk(0x24022, spec) + chunk(0x24023, b''))
    entry = chunk(0x24001, words(0, 0, 1, 0, 1, 0))
    entry += chunk(0x24025, words(0)) + chunk(0x24026, words(0)) + chunk(0x80024002, body) + group
    return chunk(0x80000001, chunk(0x80024000, entry))


def write(directory):
    for mode in ('basic', 'lifeframe', 'additive', 'depth', 'facing', 'ground', 'gpu',
                 'missing_model', 'nonanimated', 'missing_texture', 'negative_fps',
                 'zero_life', 'ascend', 'light', 'terrain', 'axis', 'layer'):
        (directory / (mode + '.bun')).write_bytes(resident(mode))
    (directory / 'nonresident.bun').write_bytes(chunk(0x80000001, chunk(0x24100, texture(0x12345678))))
    (directory / 'textures.rlt').write_bytes(texture(0x87654321))
    (directory / 'geometry.bun').write_bytes(b'Geometry is supplied by the independent generated native fixture')


def run(executable):
    with tempfile.TemporaryDirectory(prefix='charged-model-particles-') as temporary:
        directory = Path(temporary)
        write(directory)
        subprocess.run([executable, str(directory)], check=True, timeout=50)


if __name__ == '__main__':
    run(str(Path(sys.argv[1]).resolve()))
