"""Decode generated FEN animation through the native reader before playback."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from frontend_animation_fixture import animation

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='charged-fe-animation-') as folder:
    path = Path(folder) / 'generated.fen'
    payload = animation()
    for mode in ('valid','type','key-time','key-link','key-overlap','target','nan','null-keys'):
        data = bytearray(payload)
        record = 0x280
        def word(at,value):struct.pack_into('>I',data,16+at,value)
        if mode=='type': word(record+20,99)
        if mode=='key-time': word(record+28+56+12,0)
        if mode=='key-link': word(record+28+48,record+28)
        if mode=='key-overlap': word(record+24,record)
        if mode=='target':word(record+12,0x1a0)
        if mode=='nan':word(record+28,0x7fc00000)
        if mode=='null-keys':word(record+24,0xffffffff)
        path.write_bytes(data)
        run=subprocess.run([binary,'--file',str(path)],capture_output=True,text=True,timeout=30)
        assert (run.returncode==0)==(mode=='valid'),(mode,run.stdout,run.stderr)
        print(mode,run.stdout.strip().splitlines()[-1] if run.returncode==0 else run.stderr.strip())
