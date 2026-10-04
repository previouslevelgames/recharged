"""Generated multi-page bundles and real NL staged font failure cases."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from frontend_visual_fixture import font, name_hash


def bundle(base):
    one=font(base)
    texture=one[64:64+1568]
    descriptor=one[64+1568:].replace(b'PageCount 1',b'PageCount 2').replace(b'Glyph B',b'PageBreak\r\nGlyph B')
    records=[(name_hash(base+'_2'),texture),(name_hash(base),descriptor),(name_hash(base+'_1'),texture)]
    data=bytearray(96)
    struct.pack_into('>4I',data,0,32,3,1,3)
    for i,(key,payload) in enumerate(records):
        data.extend(b'\0'*(-len(data)%32))
        struct.pack_into('>3I',data,32+12*i,key,len(data)//32,len(payload))
        data.extend(payload)
    return data


def files(mode='success'):
    base='fe/fonts/eurfontheading36';bad=bundle(base)
    def word(at,value):struct.pack_into('>I',bad,at,value)
    if mode=='header':word(0,64)
    elif mode=='directory':word(8,0xffffffff)
    elif mode=='duplicate':word(56,name_hash(base+'_2'))
    elif mode=='overlap':word(60,3)
    elif mode=='descriptor-missing':word(44,0)
    elif mode=='descriptor-malformed':
        offset=struct.unpack_from('>I',bad,48)[0]*32;bad[offset:offset+3]=b'BAD'
    elif mode=='page-missing':word(32,0)
    elif mode=='page-malformed':word(96,0xffffffff)
    elif mode=='unsupported':
        offset=struct.unpack_from('>I',bad,48)[0]*32;at=bad.index(b'color',offset);bad[at:at+5]=b'split'
    elif mode=='short-header':bad=bad[:7]
    elif mode=='empty':bad=b''
    payloads={'Art/fe/fonts/eurfonttext18.res':bundle('fe/fonts/eurfonttext18'),'Art/fe/fonts/eurfontheading36.res':bad}
    if mode=='missing':del payloads['Art/fe/fonts/eurfontheading36.res']
    return payloads

if __name__=='__main__':
    executable=str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='charged-font-load-') as folder:
        root=Path(folder)
        for mode in ('success','header','directory','duplicate','overlap','descriptor-missing','descriptor-malformed','page-missing','page-malformed','unsupported','short-header','empty','missing'):
            write_disc(root/'fonts.iso',files=files(mode))
            subprocess.run([executable,str(root/'fonts.iso'),str(root),mode],check=True,timeout=45)
