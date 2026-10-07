#!/usr/bin/env python3
"""Bounded selected owned AX command arithmetic; no full DSP/kernel or ROM data."""
from pathlib import Path
import re,struct,json,sys
MASK=(1<<40)-1
def signed(x,bits):
    return (x&((1<<bits)-1))-(1<<bits) if x&(1<<(bits-1)) else x&((1<<bits)-1)
def sat16(x):return max(-32768,min(32767,x))
class Selected:
    def __init__(self,words):
        self.words=words;self.r=[0]*32;self.r[8:12]=[65535]*4
        self.a=[0,0];self.p=0;self.mem={};self.mode40=False;self.unsigned=False
        self.pc=0;self.loops=[];self.executed=0;self.factor=2
    def read(self,r):
        if r in(28,29):return self.a[r-28]&65535
        if r in(30,31):
            x=signed(self.a[r-30],40)
            return (sat16(x//65536) if self.mode40 else x>>16)&65535
        return self.r[r]
    def write(self,r,x):
        x&=65535
        if r in(28,29):self.a[r-28]=(self.a[r-28]&~65535)|x
        elif r in(30,31):
            k=r-30;self.a[k]=(signed(x,16)*65536)&MASK if self.mode40 else (self.a[k]&~(65535<<16))|(x<<16)
        else:self.r[r]=x
    def acc(self,k,x):self.a[k]=x&MASK
    def advance(self,ar,d):assert self.r[8+ar]==65535;self.r[ar]=(self.r[ar]+d)&65535
    def run(self,start,stop):
        self.pc=start
        while self.pc!=stop:
            assert self.executed<12000,'unbounded selected original routine'
            self.step()
        assert not self.loops
    def step(self):
        pc=self.pc;w=self.words[pc];after=pc+1;self.executed+=1
        old=[self.read(r)for r in range(32)];oldar=self.r[:4];ext=w&255;pending=[]
        # Secondary side effects sample registers/memory before the primary.
        if w>>12>=3:
            if ext&0xc0==0xc0:
                ar=ext&3;d=(ext>>4)&1;r=(ext>>5)&1
                pending.extend([('r',24+2*d,self.mem[oldar[ar]]),('r',25+2*r,self.mem[oldar[3]]),('ar',ar,1),('ar',3,1)])
            elif ext&0xf0==0x10:pending.append(('r',24+((ext>>2)&3),old[28+(ext&3)]))
            elif ext&0xe4==0x20:
                ar=ext&3;pending.extend([('m',oldar[ar],old[28+((ext>>3)&3)]),('ar',ar,1)])
            elif ext&0xc4==0x40:
                ar=ext&3;pending.extend([('r',24+((ext>>3)&7),self.mem[oldar[ar]]),('ar',ar,1)])
            elif ext&0xf0==0x80:
                pending.extend([('r',24+((ext>>4)&3),self.mem[oldar[0]]),('m',oldar[3],old[30+(ext&1)]),('ar',0,1),('ar',3,1)])
            elif ext&0xc2==0x80:
                # LS D bits4..5; the top two-bit pattern is10.
                pending.extend([('r',24+((ext>>4)&3),self.mem[oldar[0]]),('m',oldar[3],old[30+(ext&1)]),('ar',0,1),('ar',3,1)])
            elif ext in(0,):pass
            else:raise AssertionError(f'unknown selected extension {pc:04x}/{w:04x}')
        if w&0xffe0==0x80:self.write(w&31,self.words[pc+1]);after+=1
        elif w&0xffe0==0xc0:self.write(w&31,self.mem[self.words[pc+1]]);after+=1
        elif w&0xffe0==0xe0:self.mem[self.words[pc+1]]=old[w&31];after+=1
        elif w&0xfc00==0x1c00:self.write((w>>5)&31,old[w&31])
        elif w&0xff80 in(0x1800,0x1880,0x1900):
            ar=(w>>5)&3;self.write(w&31,self.mem[oldar[ar]])
            if w&0xff80!=0x1800:self.advance(ar,1 if w&0xff80==0x1900 else -1)
        elif w&0xff80 in(0x1a80,0x1b00):
            ar=(w>>5)&3;self.mem[oldar[ar]]=old[w&31];self.advance(ar,-1 if w&0xff80==0x1a80 else 1)
        elif w&0xf700==0x8000:pass
        elif w&0xff00 in(0x8100,0x8900):self.a[(w>>11)&1]=0
        elif w&0xff00==0x8a00:self.factor=2
        elif w&0xff00==0x8b00:self.factor=1
        elif w&0xff00==0x8d00:self.unsigned=True
        elif w&0xff00==0x8e00:self.mode40=False
        elif w&0xff00==0x8f00:self.mode40=True
        elif w in(0x4c00,0x4d00):self.acc((w>>8)&1,signed(self.a[(w>>8)&1],40)+signed(self.a[1-((w>>8)&1)],40))
        elif w&0xff00==0x5d00:self.acc(1,signed(self.a[1],40)-signed(self.a[0],40))
        elif w&0xfe00==0x4800:
            k=(w>>8)&1;ax=signed((old[26]<<16)|old[24],32)
            self.acc(k,signed(self.a[k],40)+ax)
        elif w&0xe000==0xc000:
            x=signed(old[30+((w>>12)&1)],16);y=signed(old[26+((w>>11)&1)],16)
            prior=self.p;self.p=x*y*self.factor;k=(w>>8)&1;op=(w>>9)&3
            if op==2:self.acc(k,signed(self.a[k],40)+prior)
            elif op==3:self.acc(k,prior)
            elif op!=0:raise AssertionError('unqualified MULC variant')
        elif w==0x1481:self.acc(0,self.a[0]<<1)
        elif w&0xff00==0x1100:
            count=w&255;end=self.words[pc+1];after+=1
            if count:self.loops.append([after,end,count])
            else:after=end+1
        elif w&0xe000==0xa000:
            # MULX family: source halves selectAX0/AX1; unsigned mode affects
            # low halves only. AC move/add consumes the previous product.
            s=(w>>12)&1;t=(w>>11)&1;k=(w>>8)&1;op=(w>>9)&3
            x=old[24+s*2];y=old[25+t*2]
            x=x if self.unsigned and not s else signed(x,16)
            y=y if self.unsigned and not t else signed(y,16)
            prior=self.p;self.p=x*y*self.factor
            if op==2:self.acc(k,signed(self.a[k],40)+prior)
            elif op==3:self.acc(k,prior)
            elif op!=0:raise AssertionError('unqualified multiply variant')
        elif w&0xfe00==0xf000:self.acc((w>>8)&1,self.a[(w>>8)&1]<<16)
        elif w&0xf700==0x9100:
            k=(w>>11)&1;self.acc(k,signed(self.a[k],40)>>16)
        elif w&0xff00 in(0x4e00,0x4f00):
            k=(w>>8)&1;self.acc(k,signed(self.a[k],40)+self.p)
        elif w&0xff00==0x6e00:self.acc(0,self.p)
        elif w==0x02df:raise AssertionError('unexpected selected return')
        else:raise AssertionError(f'unknown selected primary {pc:04x}/{w:04x}')
        for kind,at,value in pending:
            if kind=='r':self.write(at,value)
            elif kind=='m':self.mem[at]=value
            else:self.advance(at,value)
        if self.loops and self.loops[-1][1]==pc:
            self.loops[-1][2]-=1
            if self.loops[-1][2]:after=self.loops[-1][0]
            else:self.loops.pop()
        self.pc=after


if len(sys.argv)!=3:raise SystemExit('usage: native_ax_command_oracle.py PREPARED_DSP_CODE OUT_DIRECTORY')
source=Path(sys.argv[1]).read_text()
b=[int(x,16)for x in re.findall(r'0x([0-9a-fA-F]{2})(?![0-9a-fA-F])',source)]
words=[b[i]*256+b[i+1]for i in range(0,len(b),2)]
assert len(words)==4096

def ramp(before,after,master=False):
    d=Selected(words);d.r[4]=before;d.r[5]=after
    if master:
        d.factor=1;d.mem[0xce5]=before;d.r[25]=after;d.run(0x618,0x62f)
    else:d.run(0x4e6,0x4fb)
    return [d.mem[0xd08+i]for i in range(96)],d.executed

def put32(d,base,samples):
    for i,x in enumerate(samples):d.mem[base+2*i]=(x>>16)&65535;d.mem[base+2*i+1]=x&65535
    # Literal instruction pipelines prefetch unused cells after96 inputs.
    d.mem[base+192]=d.mem[base+193]=0
def aux(main,returned,volumes):
    d=Selected(words);d.r[6]=0x400
    for channel in range(3):
        put32(d,channel*192,main[channel]);put32(d,0x400+channel*192,returned[channel])
    for i,v in enumerate(volumes):d.mem[0xd08+i]=v
    d.mem[0xd68]=0;d.run(0x50b,0x578)
    return [[signed((d.mem[c*192+2*i]<<16)|d.mem[c*192+2*i+1],32)for i in range(96)]for c in range(3)],d.executed
def output(left,right,volumes):
    d=Selected(words);put32(d,0,left);put32(d,0xc0,right)
    for i,v in enumerate(volumes):d.mem[0xd08+i]=v
    d.mem[0xd68]=0;d.run(0x631,0x656)
    return [signed(d.mem[0x400+i],16)for i in range(192)],d.executed

out=Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
wire=bytearray(b'AXCMD345')
cases=[]
for index,(before,after) in enumerate([(0x8000,0x8000),(0x8000,0x1000),(0x1000,0x7fff),(0xffff,0),(0,0xffff),(0x8000,0xffff),(1,0x8000),(0x7fff,0)]):
    volumes,count=ramp(before,after);master,master_count=ramp(before,after,True)
    assert volumes==master,'two literal firmware ramp producers disagree'
    main=[[((i*2017+c*97)%60001)-30000 for i in range(96)]for c in range(3)]
    returned=[[((i*1291+c*101)%40001)-20000 for i in range(96)]for c in range(3)]
    if index>=4:
        main=[[[-2147483648,2147483647,-1,1][(i+c)%4]for i in range(96)]for c in range(3)]
        returned=[[[-2147483648,2147483647,0x12345678,-0x12345678][(i+c)%4]for i in range(96)]for c in range(3)]
    mixed,aux_steps=aux(main,returned,volumes)
    pcm,output_steps=output(mixed[0],mixed[1],volumes)
    cases.append(dict(before=before,after=after,ramp=volumes,main=main,returned=returned,mixed=mixed,pcm=pcm,steps=[count,master_count,aux_steps,output_steps]))
wire+=struct.pack('<I',len(cases))
for c in cases:
    wire+=struct.pack('<HH96H',c['before'],c['after'],*c['ramp'])
    for key in ['main','returned','mixed']:
        for channel in c[key]:wire+=struct.pack('<96i',*channel)
    wire+=struct.pack('<192h',*c['pcm'])
(out/'command-oracle.bin').write_bytes(wire)
(out/'command-oracle.json').write_text(json.dumps(dict(scope='owned04E6..04FA,0618..062E,050B..0577,0631..0655; controlled numeric inputs; no full kernel',cases=cases),indent=2)+'\n')
print('Owned command arithmetic oracle:',len(cases),'cases')
