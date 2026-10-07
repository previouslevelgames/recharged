"""Independent selected-ISA oracle over the owned AX SRC instructions.

This diagnostic executes original071C..0759 and076D..07AE words with a
controlled already decoded accelerator input. It is not a complete DSP
emulator/bootstrap; its
coefficient input is an explicitly synthetic conformance bank, never retail.
Instruction facts are documented in the Duddie hardware manual. No emulator
implementation, coefficient bytes or game-side source routines are imported.
"""
from pathlib import Path
import json,re,struct,sys

MASK=(1<<40)-1
def signed(x,bits):
    return (x&((1<<bits)-1))-(1<<bits) if x&(1<<(bits-1)) else x&((1<<bits)-1)
def sat16(x):return max(-32768,min(32767,x))
class SelectedSRC:
    def __init__(self,words,ratio,fraction,history,raw,taps,bank,linear=False):
        self.words=words;self.reg=[0]*32;self.acc=[0,0];self.product=0;self.mode40=True
        self.reg[2]=0x323;self.reg[8:12]=[0xffff]*4
        self.mem={0x323:ratio>>16,0x324:ratio&65535,0x325:fraction,0xce0:0x1000+bank*512}
        self.mem.update({0x326+i:x&65535 for i,x in enumerate(history)})
        self.mem.update({0x1000+i:x&65535 for i,x in enumerate(taps)})
        self.raw=raw;self.consumed=0;self.loops=[];self.pc=0x76d if linear else 0x71c;self.executed=0
        self.linear=linear;self.zero=False;self.product_shift=2;self.unsigned=False
    def read(self,r):
        if r in (28,29):return self.acc[r-28]&65535
        if r in (30,31):
            a=signed(self.acc[r-30],40)
            return (sat16(a//65536) if self.mode40 else (a>>16))&65535
        return self.reg[r]
    def write(self,r,x):
        x&=65535
        if r in (28,29):self.acc[r-28]=(self.acc[r-28]&~65535)|x
        elif r in (30,31):
            n=r-30
            self.acc[n]=(signed(x,16)*65536)&MASK if self.mode40 else (self.acc[n]&~(65535<<16))|(x<<16)
        else:self.reg[r]=x
    def advance(self,r,delta):
        old=self.reg[r];wrap=self.reg[8+r]
        assert wrap in (3,65535),'unqualified selected-ISA circular geometry'
        self.reg[r]=((old&~wrap)|((old+delta)&wrap))&65535
    def readmem(self,at):
        if at==0xffdd:
            value=self.raw[self.consumed] if self.consumed<len(self.raw) else 0
            self.consumed+=1
            return value&65535
        return self.mem[at]
    def writeacc(self,n,x):self.acc[n]=x&MASK
    def step(self):
        pc=self.pc;w=self.words[pc];self.executed+=1;after=pc+1
        old=[self.read(r) for r in range(32)]
        extension=None
        if w>>12>=3:
            ext=w&255
            if ext==0xc3:
                extension=(self.readmem(self.reg[0]),self.readmem(self.reg[3]))
            elif ext==0x12:extension=old[30]
            elif self.linear and ext==0x14:extension=old[28]
            elif self.linear and ext==0x50:extension=self.readmem(self.reg[0])
            elif self.linear and ext==0x39:extension=old[31]
            elif ext:raise AssertionError(f'unqualified extension {pc:04x}/{w:04x}')
        if w in (0x8c00,0x8a00):pass # fractional signed product mode
        elif w==0x8f00:self.mode40=True
        elif self.linear and w==0x8d00:self.unsigned=True
        elif self.linear and w==0x8b00:self.product_shift=1
        elif w&0xffe0==0x80:
            self.write(w&31,self.words[pc+1]);after+=1
        elif w&0xffe0==0xc0:
            self.write(w&31,self.readmem(self.words[pc+1]));after+=1
        elif w&0xfc00==0x1c00:self.write((w>>5)&31,old[w&31])
        elif w&0xff80 in (0x1800,0x1880,0x1900,0x1980):
            ar=(w>>5)&3;self.write(w&31,self.readmem(self.reg[ar]))
            if w&0xff80==0x1900:self.advance(ar,1)
            elif w&0xff80==0x1880:self.advance(ar,-1)
            elif w&0xff80==0x1980:self.advance(ar,signed(self.reg[4+ar],16))
        elif w&0xff80 in (0x1a80,0x1b00):
            ar=(w>>5)&3;self.mem[self.reg[ar]]=old[w&31]
            self.advance(ar,-1 if w&0xff80==0x1a80 else 1)
        elif w==0x8100:self.acc[0]=0
        elif w&0xff00==0x8900:self.acc[1]=0
        elif w==0x1577:self.acc[1]>>=9
        elif w==0x1512:self.writeacc(1,self.acc[1]<<18)
        elif w==0x001f:self.advance(3,signed(self.reg[7],16))
        elif w==0x0004:self.advance(0,-1)
        elif self.linear and w==0x0010:self.advance(0,signed(self.reg[4],16))
        elif self.linear and w in (0x7c00,0x7c50):self.writeacc(0,-signed(self.acc[0],40))
        elif self.linear and w==0xb114:self.zero=signed(self.acc[0],40)==0
        elif self.linear and w==0x0294:
            after=self.words[pc+1] if not self.zero else pc+2
        elif self.linear and w==0x029f:after=self.words[pc+1]
        elif self.linear and w in (0xb014,0xb700):
            previous=self.product
            self.product=signed(old[26],16)*(old[25] if self.unsigned else signed(old[25],16))*self.product_shift
            if w==0xb700:self.writeacc(1,previous)
        elif w in (0x1160,0x0078):
            count=(w&255) if w==0x1160 else old[24];end=self.words[pc+1];after+=1
            if count:self.loops.append([after,end,count])
            else:after=end+1
        elif w&0xff00==0x4a00:
            ax=signed((old[27]<<16)|old[25],32)
            self.writeacc(0,signed(self.acc[0],40)+ax)
        elif w==0x5000:self.writeacc(0,signed(self.acc[0],40)-signed(old[24],16)*65536)
        elif w in (0x90c3,0x97c3,0x95c3,0x9500):
            previous=self.product
            self.product=signed(old[24],16)*signed(old[26],16)*2
            if w==0x97c3:self.writeacc(1,previous)
            elif w in (0x95c3,0x9500):self.writeacc(1,signed(self.acc[1],40)+previous)
        elif w==0x4f00:self.writeacc(1,signed(self.acc[1],40)+self.product)
        elif w==0x5a00:self.writeacc(0,signed(self.acc[0],40)-signed((old[27]<<16)|old[25],32))
        else:raise AssertionError(f'unqualified original instruction {pc:04x}/{w:04x}')
        if w&255==0xc3 and extension is not None:
            self.write(26,extension[0]);self.write(24,extension[1]);self.advance(0,1);self.advance(3,1)
        elif w&255==0x12 and extension is not None:self.write(24,extension)
        elif self.linear and w&255==0x14 and extension is not None:self.write(25,extension)
        elif self.linear and w&255==0x50 and extension is not None:
            self.write(26,extension);self.advance(0,1)
        elif self.linear and w&255==0x39 and extension is not None:
            self.mem[self.reg[1]]=extension;self.advance(1,1)
        if self.loops and self.loops[-1][1]==pc:
            self.loops[-1][2]-=1
            if self.loops[-1][2]:after=self.loops[-1][0]
            else:self.loops.pop()
        self.pc=after
    def run(self):
        while self.pc!=(0x7af if self.linear else 0x75a):
            assert self.executed<15000,'selected source routine did not terminate'
            self.step()
        assert not self.loops
        return dict(pcm=[signed(self.mem[0xc60+i],16)for i in range(96)],
            fraction=self.mem[0x325],history=[signed(self.mem[0x326+i],16)for i in range(4)],
            consumed=self.consumed,instructions=self.executed)


class Selected:
    def __init__(self,words):
        self.words=words;self.r=[0]*32;self.r[8:12]=[65535]*4
        self.a=[0,0];self.p=0;self.mem={};self.mode40=False;self.unsigned=False
        self.pc=0;self.loops=[];self.executed=0
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
            if ext&0xf0==0x10:pending.append(('r',24+((ext>>2)&3),old[28+(ext&3)]))
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
        elif w&0xff00==0x8d00:self.unsigned=True
        elif w&0xff00==0x8e00:self.mode40=False
        elif w&0xff00==0x8f00:self.mode40=True
        elif w in(0x4c00,0x4d00):self.acc((w>>8)&1,signed(self.a[(w>>8)&1],40)+signed(self.a[1-((w>>8)&1)],40))
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
            prior=self.p;self.p=x*y*2
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


def envelope(words,pcm,volume,delta):
    d=Selected(words);d.mem={0x306:delta&65535,0x305:volume,0xce1:0xc60}
    d.mem.update({0xc60+i:x&65535 for i,x in enumerate(pcm)})
    d.mem[0xcc0]=0 # source routine prefetches one unused cell after96 outputs
    d.run(0x34d,0x375)
    return [signed(d.mem[0xc60+i],16)for i in range(96)],d.mem[0x305],d.executed

def mix(words,pcm,volume,delta,ramp):
    d=Selected(words);d.mode40=True;d.unsigned=True
    d.r[0]=0xc60;d.r[1]=0x360;d.r[2]=0x100;d.r[3]=0xd08 if ramp else 0x100
    d.mem={0x360:volume,0x361:delta&65535}
    d.mem.update({0xc60+i:x&65535 for i,x in enumerate(pcm)})
    d.mem[0xcc0]=0 # source routine prefetches one unused cell after96 outputs
    d.mem.update({0x100+i:0 for i in range(194)})
    d.mem[0xd68]=0 # unused pipeline prefetch beyond96 ramp volumes
    d.run(0xc21 if ramp else 0xbd1,0xc50 if ramp else 0xbea)
    return [signed(d.mem[0x100+i*2]*65536+d.mem[0x101+i*2],32)for i in range(96)],d.mem[0x360],signed(d.read(24),16),d.executed


def main():
    import argparse,hashlib
    parser=argparse.ArgumentParser(description="Generate synthetic AX hardware vectors by selected original firmware instructions")
    parser.add_argument("source",type=Path);parser.add_argument("output",type=Path)
    args=parser.parse_args();source=args.source.read_text()
    byte=[int(v,16)for v in re.findall(r"0x([0-9a-fA-F]{2})(?![0-9a-fA-F])",source)]
    words=[byte[i]*256+byte[i+1]for i in range(0,len(byte),2)]
    if len(words)!=4096 or words[0x744:0x74b]!=[0x4ac3,0x90c3,0x97c3,0x95c3,0x9500,0x4f00,0x1b3f]:
        raise RuntimeError("Prepared original AX firmware source layout differs")
    if words[0x76d:0x771]!=[0x8d00,0x8b00,0x8f00,0x195b] or words[0x791:0x7a0]!=[
        0x7c00,0xb114,0x0294,0x0799,0x191f,0x0010,0x029f,0x079e,
        0x7c50,0xb014,0x199a,0xb700,0x4f00,0x1f25,0x4a39]:
        raise RuntimeError("Prepared original linear opcode/layout differs")
    taps=[((bank+1)*1009+phase*173+tap*1999)%18000-9000 for bank in range(4)for phase in range(128)for tap in range(4)]
    wire=bytearray(b"AXSRC001")+struct.pack("<I",15)
    mixed=bytearray(b"AXMIX001")+struct.pack("<I",15)
    evidence=[];index=0
    for bank in range(3):
        for ratio,fraction in [(65536,0),(32768,0x9100),(90316,0x0240),(4*65536,0xffff),(0,0x1000)]:
            history=[1234,-2345,3456,-4567];raw=[(((i*7+3)%16)-8)*4096 for i in range(384)]
            result=SelectedSRC(words,ratio,fraction,history,raw,taps,bank).run()
            wire+=struct.pack("<IHH4h384h96hH4hH",ratio,fraction,bank,*history,*raw,*result["pcm"],result["fraction"],*result["history"],result["consumed"])
            initialVE,deltaVE=[(0xffff,17),(1,-17),(0x8000,0),(0x7400,3)][index%4]
            ve,final,steps=envelope(words,result["pcm"],initialVE,deltaVE)
            mixed+=struct.pack("<96hH",*ve,final)
            for bus in range(12):
                delta=-7 if bus%2 else 11;initialMix=0xffff if index%4==0 else 1 if index%4==1 else 10000
                pcm,volume,depop,count=mix(words,ve,(initialMix+bus*503)&65535,delta,True)
                mixed+=struct.pack("<96iHh",*pcm,volume,depop)
            evidence.append(dict(bank=bank,ratio=ratio,fraction=fraction,sourceSRCInstructions=result["instructions"],sourceVEInstructions=steps))
            index+=1
    # Independently execute the original linear instruction words, including
    # both phase branches, mixed-sign unsigned M0 products and circular ring.
    linear=bytearray(b"AXLIN502")
    linear_cases=[(65536,0),(32768,0x9100),(90316,0),(90316,0x0240),
                  (4*65536,0xffff),(0,0),(0,0xffff),(1,0xffff),
                  (65535,1),(2*65536,0x8000),(32768,0),(65536,0xffff)]
    linear+=struct.pack("<I",len(linear_cases));linear_evidence=[]
    for index,(ratio,fraction) in enumerate(linear_cases):
        history=[1234,-2345,3456,-4567] if index%2==0 else [-32768,32767,-32768,32767]
        raw=[(((i*7+3)%16)-8)*4096 for i in range(384)]
        result=SelectedSRC(words,ratio,fraction,history,raw,[],0,linear=True).run()
        linear+=struct.pack("<IHH4h384h96hH4hH",ratio,fraction,0,*history,*raw,
            *result["pcm"],result["fraction"],*result["history"],result["consumed"])
        linear_evidence.append(dict(ratio=ratio,fraction=fraction,
            consumed=result["consumed"],finalFraction=result["fraction"],sourceInstructions=result["instructions"]))
    args.output.mkdir(parents=True,exist_ok=True)
    (args.output/"linear-oracle.bin").write_bytes(linear)
    (args.output/"linear-scope.json").write_text(json.dumps(dict(
        firmwareSHA256=hashlib.sha256(bytes(byte)).hexdigest(),
        actualInstructions="076D–07AE",fullFirmwareExecution=False,
        coefficientInput=None,cases=linear_evidence),indent=2)+"\n")
    (args.output/"oracle.bin").write_bytes(wire)
    (args.output/"mix-oracle.bin").write_bytes(mixed)
    (args.output/"synthetic-drom.bin").write_bytes(struct.pack(">2048h",*taps))
    (args.output/"scope.json").write_text(json.dumps(dict(firmwareSHA256=hashlib.sha256(bytes(byte)).hexdigest(),coefficientInput="deliberate synthetic arithmetic fixture, not authentic ROM",selectedActualInstructionRanges=["071C–0759","034D–0374","0C21–0C4F"],fullFirmwareExecution=False,voices=evidence),indent=2)+"\n")

if __name__=="__main__":main()
