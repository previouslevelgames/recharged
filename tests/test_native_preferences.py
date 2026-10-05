#!/usr/bin/env python3
"""Independent fixed-format oracle; all bytes are generated, no game saves."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib


def image(*, flags=0, values=(0,5,10,1,2,3), zoom=.25, reserved=0, version=1):
    payload = struct.pack('>7If3I', flags, *values, zoom, reserved, 0, 0)
    return b'MSCPREF\0' + struct.pack('>III', version, 64, zlib.crc32(payload)) + payload


def main():
    with tempfile.TemporaryDirectory(prefix='mscharged-preferences-') as directory:
        root=pathlib.Path(directory)
        bad={
            'flags':image(flags=2), 'index':image(values=(11,5,10,1,2,3)),
            'reserved':image(reserved=1), 'zoom':image(zoom=float('nan')),
            'version':image(version=2),
        }
        for name,data in bad.items(): (root/f'bad-{name}.pref').write_bytes(data)
        subprocess.run([sys.argv[1],str(root)],check=True)
        assert (root/'format.pref').read_bytes()==image(), 'Independent big-endian/CRC oracle differs'
        assert not list(root.glob('*.pending-*')), 'Unpublished temporary file survived owner teardown'
        print('Independent Python native format/CRC oracle passed')

if __name__=='__main__': main()
