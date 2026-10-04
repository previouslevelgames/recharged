#!/usr/bin/env python3
import pathlib
import struct
import subprocess
import sys
import tempfile
from test_particle_simulation import resident
from effects_registry_fixture import chunk, words


def render_files(folder):
    header = bytearray(32)
    struct.pack_into('>II',header,0,1,3)
    header[8:12] = bytes((8,8,8,8))
    struct.pack_into('>HH',header,14,4,4)
    pixels = bytearray(64)
    for i in range(16): pixels[i*2:i*2+2] = b'\xff\xff'; pixels[32+i*2:34+i*2] = b'\xff\xff'
    entry = bytes(header+pixels)
    textures = words(0x50544c47,1,0,0)+words(0x12345678,0,len(entry),0)+entry
    gradient=bytearray(pixels)
    for i in range(16):
        gradient[i*2+1]=(i%4)*64;gradient[32+i*2]=(i//4)*64;gradient[33+i*2]=0
    uv_entry=bytes(header+gradient)
    uv_textures=words(0x50544c47,1,0,0)+words(0x12345678,0,len(uv_entry),0)+uv_entry
    (folder/'uvnonresident.bun').write_bytes(chunk(0x80000001,chunk(0x24100,uv_textures)))
    (folder/'nonresident.bun').write_bytes(chunk(0x80000001,chunk(0x24100,textures)))
    (folder/'geometry.bun').write_bytes(b'opaque geometry')
    (folder/'textures.rlt').write_bytes(textures.replace(words(0x12345678),words(0x87654321),1))
    for mode in ('basic','atlas9'):
        (folder/(mode+'.bun')).write_bytes(resident(mode))
    # Locate real synthetic chunk records independently; change serialized scalar
    # and colour values only, preserving all container sizes/pointer sentinels.
    def walk(data,a,z):
        while a<z:
            kind,size=struct.unpack_from('>II',data,a);p=a+8;e=p+size
            yield kind,p,e
            if kind&0x80000000:yield from walk(data,p,e)
            a=(e+3)&-4
    for mode in ('normal','additive','infront','order','transparent','uv'):
        data=bytearray(resident('basic'))
        for kind,p,e in walk(data,0,len(data)):
            if kind==0x24003:
                struct.pack_into('>f',data,p+4,10)
                struct.pack_into('>f',data,p+8,0)
                struct.pack_into('>f',data,p+0x10,1)
                data[p+0x35]=int(mode=='additive');data[p+0x37]=int(mode=='infront')
                struct.pack_into('>I',data,p+0x3c,4 if mode=='uv' else 1)
                for i in range(25):
                    colour=(200,100,50,0 if mode=='transparent' else 128)
                    if mode=='uv':colour=(255,255,255,255)
                    if mode=='order':colour=(200,0,0,128) if i<9 else (0,0,200,128)
                    data[p+0x78+i*4:p+0x7c+i*4]=bytes(colour)
            elif kind==0x24005:
                # Emission4, size2, scale1; all movement/radius/rotation zero.
                values=(4,2,1,0,0,0,0,0)
                index=getattr(walk,'prop',0);struct.pack_into('>f',data,p+4,values[index]);walk.prop=(index+1)%8
        (folder/(mode+'.bun')).write_bytes(data)


def run(executable):
    with tempfile.TemporaryDirectory(prefix='charged-particle-render-') as temporary:
        folder=pathlib.Path(temporary);render_files(folder)
        subprocess.run([executable,str(folder)],check=True)
if __name__=='__main__':run(sys.argv[1])
