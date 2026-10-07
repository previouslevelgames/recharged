"""Selected owned AX009F..021F Setup arithmetic; no ROM/game emulation.

Independent small instruction observer for controlled Studio inputs. Instruction
formats checked against Duddie's hardware manual, not copied emulator code or
pseudocode. Both native/remote ramps are observed; this does not qualify remote
output/mixing or a complete firmware kernel.
"""
from pathlib import Path
import hashlib,json,re,struct,sys
MASK=(1<<40)-1
def signed(value,width):
    value &= (1<<width)-1
    return value-(1<<width) if value&(1<<(width-1)) else value

def words(path):
    raw=bytes(int(x,16) for x in re.findall(r"0x([0-9a-fA-F]{2})(?![0-9a-fA-F])",Path(path).read_text()))
    assert len(raw)==8192
    return [int.from_bytes(raw[i:i+2],"big") for i in range(0,len(raw),2)]

class OwnedSetup:
    def __init__(self,image,records):
        self.image=image;self.r=[0]*32;self.r[8:12]=[65535]*4
        self.r[1]=0xd08;self.r[27]=95;self.r[26]=192
        self.a=[0,0];self.mem={};self.pc=0xa2;self.loops=[];self.steps=0;self.zero=True
        for i,(value,delta) in enumerate(records):
            self.mem[0xd08+i*3]=(value>>16)&65535
            self.mem[0xd09+i*3]=value&65535
            self.mem[0xd0a+i*3]=delta&65535
    def read(self,reg):
        if reg in(28,29):return self.a[reg-28]&65535
        if reg in(30,31):return max(-32768,min(32767,signed(self.a[reg-30],40)//65536))&65535
        return self.r[reg]
    def write(self,reg,value):
        value &=65535
        if reg in(28,29):self.a[reg-28]=(self.a[reg-28]&~65535)|value
        elif reg in(30,31):self.a[reg-30]=(signed(value,16)*65536)&MASK
        else:self.r[reg]=value
    def step(self):
        pc=self.pc;op=self.image[pc];after=pc+1;self.steps+=1;old=[self.read(r) for r in range(32)]
        if op==0x8f00:pass # Actual source enables40-bit mode.
        elif op&0xffe0==0x0080:self.write(op&31,self.image[pc+1]);after+=1
        elif op&0xff80==0x1900:
            ar=(op>>5)&3;self.write(op&31,self.mem[self.r[ar]]);self.r[ar]=(self.r[ar]+1)&65535
        elif op==0xb179:
            self.zero=signed(self.a[0],40)==0
            # Simultaneous L loads AC1.M from the actual next signed delta.
            self.write(31,self.mem[self.r[1]]);self.r[1]=(self.r[1]+1)&65535
        elif op==0x0294:after=self.image[pc+1] if not self.zero else pc+2
        elif op==0x029f:after=self.image[pc+1]
        elif op==0x9900:self.a[1]=(signed(self.a[1],40)>>16)&MASK
        elif op==0x4c00:self.a[0]=(signed(self.a[0],40)+signed(self.a[1],40))&MASK
        elif op&0xff80==0x1b00:
            ar=(op>>5)&3;self.mem[self.r[ar]]=old[op&31];self.r[ar]=(self.r[ar]+1)&65535
        elif op in(0x005a,0x007b):
            count=old[op&31];start=pc+1;end=pc+1
            if op==0x007b:start=pc+2;end=self.image[pc+1]
            after=start
            if count:self.loops.append([start,end,count])
            else:after=end+1
        else:raise AssertionError(f'unqualified owned Setup instruction{pc:04x}/{op:04x}')
        if self.loops and self.loops[-1][1]==pc:
            self.loops[-1][2]-=1
            if self.loops[-1][2]:after=self.loops[-1][0]
            else:self.loops.pop()
        self.pc=after
    def run(self):
        while self.pc!=0x6f:
            assert self.steps<20000
            self.step()
        assert not self.loops and self.r[1]==0xd08+60
        buffers=[0,0xc0,0x180,0x400,0x4c0,0x580,0x640,0x700,0x7c0,0x880,0x940,0xa00,
                 0x240,0xac0,0x264,0xae4,0x288,0xb08,0x2ac,0xb2c]
        output=[]
        for i,base in enumerate(buffers):
            n=96 if i<12 else 18
            output.append([signed((self.mem[base+j*2]<<16)|self.mem[base+j*2+1],32) for j in range(n)])
        return output

def generate(source,out):
    image=words(source);out=Path(out);out.mkdir(parents=True,exist_ok=True)
    values=[0,1,-1,32767,-32768,0x7fffffff,-0x80000000,65535,-65536,2048,-2048,0x12345678]
    deltas=[0,1,-1,20,-20,32767,-32768,7,-11]
    cases=[];wire=bytearray(b'AXDP399\0')
    for case in range(24):
        records=[(values[(case+i)%len(values)],deltas[(case*3+i)%len(deltas)]) for i in range(20)]
        machine=OwnedSetup(image,records);samples=machine.run()
        cases.append(dict(records=records,samples=samples,executed=machine.steps))
    wire+=struct.pack('<I',len(cases))
    for c in cases:
        for (value,delta),samples in zip(c['records'],c['samples']):
            wire+=struct.pack('<ih',value,delta)+struct.pack('<'+'i'*len(samples),*samples)
    (out/'depop-oracle.bin').write_bytes(wire)
    (out/'scope.json').write_text(json.dumps(dict(source_sha256=hashlib.sha256(Path(source).read_bytes()).hexdigest(),
        scope='Owned00A2..021F after real Studio DMA completion,20Studio records, twelve96/eight18-frame ramps; no remote output/ROM/kernel readiness',
        cases=len(cases),executed=[c['executed'] for c in cases]),indent=2)+'\n')
    print('owned Setup oracle:',len(cases),'cases')
if __name__=='__main__':
    if len(sys.argv)!=3:raise SystemExit('usage: native_ax_depop_oracle.py DSPCode.c OUTPUT_DIR')
    generate(*sys.argv[1:])
