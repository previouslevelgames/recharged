#!/usr/bin/env python3
"""Independent synthetic Wii effects assets used by real NL boot services."""
from pathlib import Path
import struct,subprocess,sys,zlib,tempfile
from effects_registry_fixture import chunk,entry,resident,textures,words
from disc_fixture import write_disc

def geometry():
    p=bytearray(8);struct.pack_into('>I',p,0,0xabcdef01);p[6]=3
    g=chunk(0x1b016,p)+chunk(0x1b007,struct.pack('>3H',0,2,1))
    vertices=struct.pack('>15f',*range(-4,5),-1,-.5,0,.5,1,1.5)+bytes(range(10,22))
    g+=chunk(0x1b006,vertices)
    streams=bytearray(24);streams[5:7]=bytes((12,1));struct.pack_into('>I',streams,8,36);streams[13:15]=bytes((8,4));struct.pack_into('>I',streams,16,60);streams[21:23]=bytes((4,3));g+=chunk(0x1b005,streams)
    packet=bytearray(48);struct.pack_into('>I',packet,4,3);struct.pack_into('>H',packet,8,3);packet[11]=3;struct.pack_into('>I',packet,16,0x19065bf6);g+=chunk(0x1b004,packet)
    matrix=struct.pack('>16f',*(1 if i%5==0 else 0 for i in range(16)));g+=chunk(0x1b002,matrix)+chunk(0x1b003,words(0x10203040,1,0))
    g+=chunk(0x8001b008,chunk(0x1b00b,b'')+chunk(0x1b00a,b'')+chunk(0x1b00c,words(0,0x100,1)))
    anim=words(0x10203040,2,3,12,1,1,1)+struct.pack('>18f',*(i-11.5 for i in range(18)))
    g+=chunk(0x8001b200,chunk(0x1b201,anim))
    return chunk(0x80000001,chunk(0x8001b000,g))

def script():
    code=[8<<11|4,8<<11|41,8<<11|28,8<<11|1,10<<11]
    return words(0xe11c2112,1,0,0,0,len(code)*2,0,0,0,0,0,0,0,0,0,0,0,0)+struct.pack('>IIHBB',0xb53474ff,0,2,0,0)+struct.pack('>'+str(len(code))+'H',*code)

def fixture():
    nr=chunk(0x80000001,chunk(0x24100,textures()))
    return {'art/effects/effects.bun':resident(entry()),'art/effects/effectsNonRes.bun.zlib':words(len(nr))+zlib.compress(nr),
      'art/objects/effectsgeometry.bun':geometry(),'art/objects/effectsgeometrytextures.rlt':textures(0xabcdef01)}
def main(executable, out):
    out.mkdir(parents=True,exist_ok=True)
    (out/'boot.byte_code').write_bytes(script())
    for mode in ('valid','missing','truncated_geometry','bad_compressed','user_factory','texture_collision','finish_before_begin','double_begin'):
        data=fixture();code=script()
        if mode=='missing':del data['art/objects/effectsgeometry.bun']
        if mode=='truncated_geometry':data['art/objects/effectsgeometry.bun']=geometry()[:-1]
        if mode=='bad_compressed':data['art/effects/effectsNonRes.bun.zlib']=words(1000)+b'not-zlib'
        if mode=='user_factory':data['art/effects/effects.bun']=resident(entry(user=True))
        if mode=='texture_collision':
            nr=chunk(0x80000001,chunk(0x24100,textures(0xabcdef01,99)))
            data['art/effects/effectsNonRes.bun.zlib']=words(len(nr))+zlib.compress(nr)
        if mode=='finish_before_begin':code=code[:-10]+struct.pack('>5H',8<<11|4,8<<11|28,8<<11|41,8<<11|1,10<<11)
        if mode=='double_begin':code=code[:-10]+struct.pack('>5H',8<<11|4,8<<11|41,8<<11|41,8<<11|1,10<<11)
        image=out/(mode+'.iso');bytecode=out/(mode+'.byte_code')
        write_disc(image,files=data,fst_capacity=0x800);bytecode.write_bytes(code)
        subprocess.run([str(executable),str(image),str(bytecode),*(['failure'] if mode!='valid' else [])],check=True,timeout=30)
        print(mode+' passed',flush=True)
if __name__=='__main__':
    executable=Path(sys.argv[1]).resolve()
    if len(sys.argv)>2:main(executable,Path(sys.argv[2]))
    else:
        with tempfile.TemporaryDirectory(prefix='charged-boot-effects-') as temporary:main(executable,Path(temporary))
