"""Raw Wii bundle oracles for the original NL reader and callback lifetime."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from frontend_visual_fixture import font, name_hash
from test_frontend_font_load import bundle as font_bundle


def run(executable, folder, owned=None):
    def execute(arguments):
        subprocess.run([executable, *map(str, arguments)], check=True, timeout=45)

    def manifest(raw, names, name):
        sector, count, directory, data = struct.unpack_from('>4I', raw)
        words = [struct.unpack_from('>3I', raw, directory*sector+12*i)
                 for i in range(count)]
        text = f'{sector} {count} {directory} {data}\n'
        text += ''.join(f'{key} {block} {size} {names.get(key, "-")}\n'
                        for key, block, size in words)
        output = folder/(name+'.oracle')
        output.write_text(text)
        return output

    def fixture(raw, names, name, mode='ordinary'):
        source = folder/(name+'.res')
        source.write_bytes(raw)
        iso = folder/(name+'.iso')
        write_disc(iso, files={'Art/bundle.res': raw})
        oracle = manifest(raw, names, name) if mode != 'malformed' else folder/'unused'
        execute([iso, source, oracle, '/Art/bundle.res', mode])

    sector = 32
    sizes = [33, 0, 1, 17, 31, 32, 63, 64]
    raw = bytearray(128)
    struct.pack_into('>4I', raw, 0, sector, len(sizes), 1, 4)
    names = {}
    for index, size in enumerate(sizes):
        name = f'test/payload_{index}'
        key = name_hash(name)
        names[key] = name
        raw.extend(b'\0'*(-len(raw) % sector))
        struct.pack_into('>3I', raw, 32+12*index, key, len(raw)//sector, size)
        raw.extend(bytes((index*29+j*37+11) & 255 for j in range(size)))
    fixture(raw, names, 'boundary-sizes', 'lifecycle')
    base = 'fe/fonts/eurfontheading36'
    names = {name_hash(base+suffix): base+suffix for suffix in ('', '_1', '_2')}
    fixture(font_bundle(base), names, 'font-multipage')
    base = 'fe/fonts/eurfonttext18'
    names = {name_hash(base+suffix): base+suffix for suffix in ('', '_1')}
    fixture(font(base), names, 'font-singlepage')
    fixture(struct.pack('>4I', 32, 0, 0, 0), {}, 'zero-directory')
    for name, offset, word in [
        ('zero-sector', 0, 0), ('directory-outside', 8, 0xffffffff),
        ('count-overflow', 4, 0xffffffff), ('entry-outside', 36, 0xffffffff),
        ('length-outside', 40, 0xffffffff),
    ]:
        bad = bytearray(raw)
        struct.pack_into('>I', bad, offset, word)
        fixture(bad, {}, name, 'malformed')
    fixture(raw[:7], {}, 'short-header', 'malformed')
    fixture(b'', {}, 'empty-file', 'malformed')
    iso = folder/'missing.iso'
    write_disc(iso, files={'Art/other.res': b'x'})
    execute([iso, folder/'unused', folder/'unused', '/Art/bundle.res', 'missing'])
    if owned:
        for filename in ('eurfonttext18', 'eurfontheading36'):
            nl_path = '/Art/fe/fonts/'+filename+'.res'
            source = folder/(filename+'.owned.res')
            execute(['--dump', owned, nl_path, source])
            base = 'fe/fonts/'+filename
            names = {name_hash(base+suffix): base+suffix
                     for suffix in ('', '_1', '_2', '_3', '_4')}
            oracle = manifest(source.read_bytes(), names, filename+'-owned')
            execute([owned, source, oracle, nl_path, 'ordinary'])


if __name__ == '__main__':
    executable = str(Path(sys.argv[1]).resolve())
    owned = str(Path(sys.argv[3]).resolve()) if len(sys.argv) == 4 and sys.argv[2] == '--owned' else None
    with tempfile.TemporaryDirectory(prefix='charged-original-bundle-') as folder:
        run(executable, Path(folder), owned)
