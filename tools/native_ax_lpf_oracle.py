#!/usr/bin/env python3
"""Selected owned06AB..06C2 AX LPF instructions, controlled source PB values.

The oracle executes the owned firmware words and parallel operand transfers.
It does not use the native filter recurrence, supply ROM, or initialize AX.
Instruction definitions: Duddie's primary GameCube DSP hardware manual.
"""
from pathlib import Path
import argparse, hashlib, json, re, struct
from native_ax_voice_oracle import signed, sat16, MASK, mix

class SelectedLPF:
    def __init__(self,words,pcm,history,a0,b0):
        self.words=words;self.r=[0]*32;self.r[8:12]=[65535]*4
        self.r[0]=self.r[1]=0xc60;self.r[3]=0x32e
        self.a=[0,0];self.product=0;self.pc=0x6ab;self.loops=[];self.steps=0
        self.mem={0x32e:history&65535,0x32f:a0&65535,0x330:b0&65535}
        self.mem.update({0xc60+i:x&65535 for i,x in enumerate(pcm)})
        self.mem[0xcc0]=0 # actual harmless prefetch following the96 samples
    def read(self,r):
        if r in(28,29):return self.a[r-28]&65535
        if r in(30,31):return sat16(signed(self.a[r-30],40)//65536)&65535
        return self.r[r]
    def write(self,r,value):
        value&=65535
        if r in(30,31):self.a[r-30]=(signed(value,16)*65536)&MASK
        else:self.r[r]=value
    def advance(self,r,n):
        assert self.r[8+r]==65535
        self.r[r]=(self.r[r]+n)&65535
    def run(self):
        while self.pc!=0x6c2:
            assert self.steps<500,'owned LPF failed to finish its96 samples'
            pc=self.pc;w=self.words[pc];after=pc+1;self.steps+=1
            old=[self.read(r)for r in range(32)];pending=[]
            if w>=0x3000:
                e=w&255
                if e==0xe1: # LD AX0.H,@AR1 and AX1.L,@AR3, ++both
                    pending=[('r',26,self.mem[self.r[1]]),('r',25,self.mem[self.r[3]]),('a',1,1),('a',3,1)]
                elif e==0x4f: # LN AX1.L,@AR3, addIX3
                    pending=[('r',25,self.mem[self.r[3]]),('a',3,signed(self.r[7],16))]
                elif e in(0x9a,0x9b): # SLM AC0/1.M,@AR0, AX1.L,@AR3
                    pending=[('m',self.r[0],old[30+(e&1)]),('r',25,self.mem[self.r[3]]),('a',0,1),('a',3,signed(self.r[7],16))]
                elif e==0x30:pending=[('m',self.r[0],old[30]),('a',0,1)]
                elif e:raise AssertionError(f'unqualified extension{pc:04x}/{w:04x}')
            if w&0xffe0==0x80:self.write(w&31,self.words[pc+1]);after+=1
            elif w&0xfc00==0x1c00:self.write((w>>5)&31,old[w&31])
            elif w&0xff80==0x1900:
                ar=(w>>5)&3;self.write(w&31,self.mem[self.r[ar]]);self.advance(ar,1)
            elif w&0xff80==0x1b00:
                ar=(w>>5)&3;self.mem[self.r[ar]]=old[w&31];self.advance(ar,1)
            elif w&0xf700==0x8000:pass # NX parallel-only
            elif w&0xe000==0xa000: # signed fractional MULX/MULXMV
                s=(w>>12)&1;t=(w>>11)&1;k=(w>>8)&1;op=(w>>9)&3
                prior=self.product
                self.product=(signed(old[24+s*2],16)*signed(old[25+t*2],16)*2)&MASK
                if op==3:self.a[k]=prior
                elif op:raise AssertionError('unqualified multiply')
            elif w&0xfc00==0xe000: # signed fractional MADDX
                s=(w>>9)&1;t=(w>>8)&1
                self.product=(self.product+signed(old[24+s*2],16)*signed(old[25+t*2],16)*2)&MASK
            elif w&0xfe00==0x6e00:self.a[(w>>8)&1]=self.product
            elif w&0xff00==0x1100:
                self.loops.append([pc+2,self.words[pc+1],w&255]);after+=1
            else:raise AssertionError(f'unqualified primary{pc:04x}/{w:04x}')
            for kind,at,value in pending:
                if kind=='r':self.write(at,value)
                elif kind=='m':self.mem[at]=value
                else:self.advance(at,value)
            if self.loops and self.loops[-1][1]==pc:
                self.loops[-1][2]-=1
                if self.loops[-1][2]:after=self.loops[-1][0]
                else:self.loops.pop()
            self.pc=after
        assert not self.loops and self.r[0]==0xcc0
        return [signed(self.mem[0xc60+i],16)for i in range(96)],signed(self.mem[0x32e],16),self.steps

def main():
    p=argparse.ArgumentParser();p.add_argument('firmware',type=Path);p.add_argument('mix',type=Path);p.add_argument('output',type=Path);args=p.parse_args()
    b=bytes(int(v,16)for v in re.findall(r'0x([0-9a-fA-F]{2})(?![0-9a-fA-F])',args.firmware.read_text()))
    words=[int.from_bytes(b[i:i+2],'big')for i in range(0,len(b),2)]
    assert words[0x6ab:0x6c3]==[0x0087,0xffff,0x1c83,0x197e,0x80e1,0xb04f,0x1f5e,0xe2e1,0xb64f,0x1f5e,0xe2e1,0x112f,0x06bd,0xb79a,0x1f5f,0xe2e1,0xb69b,0x1f5e,0xe2e1,0x6f30,0x1b1f,0x1c64,0x1b7f,0x02df]
    source=args.mix.read_bytes();assert source[:8]==b'AXMIX001' and len(source)==12+15*(194+12*388)
    cases=[(0,0x7fff,0),(32767,0,0x7fff),(-32768,0x4000,0x4000),(12345,0x7777,0x6666),(-12345,0xffff,0x8000),(0,0,0),(99,1,0xffff)]
    wire=bytearray(b'AXLPF373')+struct.pack('<I',15);evidence=[]
    for index in range(15):
        pcm=list(struct.unpack_from('<96h',source,12+index*(194+12*388)))
        h,a0,b0=cases[index%len(cases)];out,history,steps=SelectedLPF(words,pcm,h,a0,b0).run()
        wire+=struct.pack('<HhHH96hh',2 if index==14 else 1,h,a0,b0,*out,history)
        for bus in range(12):
            delta=-7 if bus%2 else 11;initial=0xffff if index%4==0 else 1 if index%4==1 else 10000
            values,vol,depop,count=mix(words,out,(initial+bus*503)&65535,delta,True)
            wire+=struct.pack('<96iHh',*values,vol,depop)
        evidence.append(dict(history=h,a0=a0,b0=b0,steps=steps,finalHistory=history))
    # Independent opcode vectors cover sign, clipping, fractional truncation
    # and continuation history across two complete96-sample jobs.
    edges=[]
    for h,a0,b0 in cases:
        pcm=[-32768,32767,-1,1]*24
        out,last,n=SelectedLPF(words,pcm,h,a0,b0).run()
        continued,end,m=SelectedLPF(words,pcm,last,a0,b0).run()
        edges.append(dict(history=h,a0=a0,b0=b0,pcm=pcm,result=out,final=last,continued=continued,end=end))
    args.output.write_bytes(wire)
    args.output.with_suffix('.json').write_text(json.dumps(dict(firmwareSHA256=hashlib.sha256(b).hexdigest(),selectedRange='06AB–06C2',fullFirmwareExecution=False,cases=evidence,edges=edges),indent=2)+'\n')
    print('Owned AX LPF oracle:15 source-envelope frames,7 two-frame sign/saturation edges')
if __name__=='__main__':main()
