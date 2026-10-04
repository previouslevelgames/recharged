"""Generated complete multi-emitter groups; no retail asset bytes."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from effects_registry_fixture import chunk, words
from test_particle_simulation import resident
from test_particle_render import render_files


def pieces(data):
    at=0
    while at<len(data):
        kind,size=struct.unpack_from('>II',data,at);yield kind,data[at+8:at+8+size];at=(at+8+size+3)&-4


def multi(data, bad=False, lingering=False, gpu=False, count=2, group=0x81f2a311):
    _,top=next(pieces(data));_,entry=next(pieces(top));parts=list(pieces(entry));template=next(p for k,p in parts if k==0x80024002)
    authored_group=next(p for k,p in parts if k==0x80024020);gp=list(pieces(authored_group));spec=next(p for k,p in gp if k==0x24022)
    second=bytearray(spec);struct.pack_into('>I',second,4,1)
    if bad:struct.pack_into('>I',second,8,1)
    def coloured(template, colour):
        result=b''
        for k,p in pieces(template):
            if k==0x24003:
                p=bytearray(p)
                for i in range(25):p[0x78+4*i:0x7c+4*i]=bytes(colour)
            result+=chunk(k,p)
        return result
    left=coloured(template,(200,0,0,128)) if gpu else template
    right=coloured(template,(0,0,200,128)) if gpu else template
    body=chunk(0x24001,words(0,0,2,0,1,0))+chunk(0x24025,words(0,0))+chunk(0x24026,words(0))
    body+=chunk(0x80024002,left)+chunk(0x80024002,right)
    body+=chunk(0x80024020,chunk(0x24021,words(group,0,count,lingering,0,0,0))+chunk(0x24022,b"".join(spec if i%2==0 else second for i in range(count)))+chunk(0x24023,b''))
    return chunk(0x80000001,chunk(0x80024000,body))


def files(folder):
    render_files(folder)
    (folder/'multi.bun').write_bytes(multi(resident()))
    (folder/'bad.bun').write_bytes(multi(resident(),bad=True))
    (folder/'linger.bun').write_bytes(multi(resident(),lingering=True))
    (folder/'gpu.bun').write_bytes(multi((folder/'normal.bun').read_bytes(),gpu=True))

if __name__=='__main__':
    with tempfile.TemporaryDirectory(prefix='charged-particle-controllers-') as temporary:
        folder=Path(temporary);files(folder);subprocess.run([str(Path(sys.argv[1]).resolve()),str(folder)],check=True)
