"""Independent world-record/schedule fixture; no retail bytes."""
from pathlib import Path
import struct,subprocess,sys,tempfile,re
from effects_registry_fixture import chunk,words
from test_particle_controller import files as controller_fixture

def world(records):
    body=words(len(records),0,0,0)
    for identity,mode,count,probability,interval in records:
        item=bytearray(0xa0)
        struct.pack_into('>IIII',item,0,0xdeadbeef,identity,0x109,1)
        struct.pack_into('>III',item,0x10,0x12345678,0xffffffff,0xcafebabe)
        struct.pack_into('>16f',item,0x20,*[1 if i%5==0 else 0 for i in range(16)])
        struct.pack_into('>ff',item,0x60,interval,-1)
        struct.pack_into('>IIII',item,0x70,0x81f2a311,probability,count&0xffffffff,mode&0xffffffff)
        # Saved runtime bytes are poison: native initialization must overwrite,
        # not validate or trust original vtables/pointers/counters/radius.
        item[0x80:0x9c]=bytes([0xff])*28
        item[0x9c]=0
        body+=item
    return chunk(0x80000001,chunk(0x26000,body))

def fingerprint(data):
    result=14695981039346656037
    count=struct.unpack_from('>I',data,16)[0]
    for n in range(count):
        at=32+n*0xa0
        offsets=(4,0x70,12,0x74,0x78,0x7c,0x14,0x60,0x64,*range(0x20,0x60,4))
        payload=b''.join(data[at+i:at+i+4] for i in offsets)+struct.pack('>II',0,data[at+0x9c])
        for byte in payload:result=((result^byte)*1099511628211)&0xffffffffffffffff
    return result

def write(path):
    (path/'world.res').write_bytes(world([(1,0,-1,100,1),(2,39,2,100,1),(3,7,1,50,1)]))
    (path/'single.res').write_bytes(world([(1,39,1,100,1)]))
    controller_fixture(path)

def run(executable):
    with tempfile.TemporaryDirectory(prefix='charged-world-effects-') as name:
        folder=Path(name);write(folder)
        result=subprocess.run([executable,str(folder)],check=True,timeout=45,capture_output=True,text=True)
        print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
        actual=re.findall(r'world-effects-fingerprint=([0-9a-f]+)',result.stdout)
        expected=fingerprint((folder/'world.res').read_bytes())
        assert len(actual)==3 and all(int(v,16)==expected for v in actual), (actual,expected)
        print('Independent raw world-record fingerprint matches every retained field')
if __name__=='__main__':run(str(Path(sys.argv[1]).resolve()))
