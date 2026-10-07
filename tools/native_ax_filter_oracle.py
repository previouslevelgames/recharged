"""Independent80-digit native filter design and original PB arithmetic vectors.

No hardware coefficient table is read. The selected original AX instructions
only qualify arithmetic with these explicitly self-generated native inputs.
"""
from pathlib import Path
from decimal import Decimal, localcontext, ROUND_HALF_UP
import argparse, hashlib, json, re, struct
from native_ax_voice_oracle import SelectedSRC, envelope, mix

PI=Decimal('3.14159265358979323846264338327950288419716939937510582097494459230781640628620899')

def sinc(value):
    if not value:
        return Decimal(1)
    angle=PI*value
    term=angle
    total=term
    for n in range(1,100):
        term *= -angle*angle/Decimal((2*n)*(2*n+1))
        total += term
        if abs(term)<Decimal('1e-75'):
            break
    else:
        raise AssertionError('native mathematical sine oracle did not converge')
    return total/angle

def coefficients():
    taps=[]
    with localcontext() as context:
        context.prec=80
        for bank in range(3):
            cutoff=Decimal(bank+2)/8
            for phase in range(128):
                center=Decimal(1)+Decimal(phase)/128
                weights=[]
                for tap in range(4):
                    distance=Decimal(tap)-center
                    weights.append(2*cutoff*sinc(2*cutoff*distance)*sinc(distance/2))
                total=sum(weights)
                row=[max(-32768,min(32767,int((weight*32768/total).to_integral_value(rounding=ROUND_HALF_UP))))for weight in weights]
                residual=32768-sum(row)
                while residual:
                    choices=[n for n in range(4)if (row[n]<32767 if residual>0 else row[n]>-32768)]
                    selected=min(choices,key=lambda n:(abs(Decimal(n)-center),-n))
                    delta=1 if residual>0 else -1
                    row[selected]+=delta;residual-=delta
                assert sum(row)==32768
                taps.extend(row)
    return taps+[0]*512 # fourth bank is unused/invalid, not a generated fallback

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    from math import cos, sin, pi
    raw=bytes(int(x,16)for x in re.findall(r'0x([0-9a-fA-F]{2})(?![0-9a-fA-F])',args.source.read_text()))
    assert len(raw)==8192
    words=[raw[n]*256+raw[n+1]for n in range(0,len(raw),2)]
    taps=coefficients()
    args.output.mkdir(parents=True,exist_ok=True)
    wire=bytearray(b'AXSRC001')+struct.pack('<I',15)
    mixed=bytearray(b'AXMIX001')+struct.pack('<I',15)
    index=0
    for bank in range(3):
        for ratio,fraction in [(65536,0),(32768,0x9100),(90316,0x0240),(4*65536,0xffff),(0,0x1000)]:
            history=[1234,-2345,3456,-4567]
            source=[(((n*7+3)%16)-8)*4096 for n in range(384)]
            result=SelectedSRC(words,ratio,fraction,history,source,taps,bank).run()
            wire+=struct.pack('<IHH4h384h96hH4hH',ratio,fraction,bank,*history,*source,*result['pcm'],result['fraction'],*result['history'],result['consumed'])
            volume,delta=[(0xffff,17),(1,-17),(0x8000,0),(0x7400,3)][index%4]
            ve,final,_=envelope(words,result['pcm'],volume,delta)
            mixed+=struct.pack('<96hH',*ve,final)
            for bus in range(12):
                initial=0xffff if index%4==0 else 1 if index%4==1 else 10000
                values,volume,depop,_=mix(words,ve,(initial+bus*503)&65535,-7 if bus%2 else 11,True)
                mixed+=struct.pack('<96iHh',*values,volume,depop)
            index+=1
    continuation=bytearray(b'AXCONT01')+struct.pack('<I',3)
    for ratio,fraction,loop in [(32768,0x9100,0),(90316,0x0240,0),(4*65536,0xffff,1)]:
        continuation+=struct.pack('<IHH',ratio,fraction,loop)
        history=[0]*4
        used=0
        for frame in range(3):
            total=(fraction+96*ratio)//65536
            source_pcm=[(((n*7+3)%16)-8)*4096 for n in range(384)]
            source=[source_pcm[(used+n)%384] if loop or used+n<384 else 0 for n in range(total)]
            result=SelectedSRC(words,ratio,fraction,history,source,taps,0).run()
            decoded=total if loop else max(0,min(total,384-used))
            loops=(used+total)//384-used//384 if loop else 0
            used+=decoded
            fraction=result['fraction'];history=result['history']
            running=1 if loop or used<384 else 0
            continuation+=struct.pack('<96hH4hIHH',*result['pcm'],fraction,*history,decoded,loops,running)
    (args.output/'native-continuation.bin').write_bytes(continuation)
    (args.output/'native-filter-rows.bin').write_bytes(struct.pack('>2048h',*taps))
    (args.output/'oracle.bin').write_bytes(wire)
    (args.output/'mix-oracle.bin').write_bytes(mixed)
    # Frequency response documents native quality/delay, not guessed Wii parity.
    response=[]
    for bank in range(3):
        for phase in (0,32,64,96,127):
            row=taps[bank*512+phase*4:bank*512+phase*4+4]
            gains=[]
            for frequency in (0,.125,.25,.375,.5):
                real=sum(row[n]*cos(2*pi*frequency*n)/32768 for n in range(4))
                imag=sum(row[n]*sin(2*pi*frequency*n)/32768 for n in range(4))
                gains.append((real*real+imag*imag)**.5)
            response.append(dict(bank=bank,phase=phase,gain_at_normalized_frequencies=gains))
    (args.output/'scope.json').write_text(json.dumps(dict(policy='NativeWindowedSinc4TapV1',authenticWiiCoefficients=False,precision=80,formula='normalized2fc*sinc(2fc*(tap-center))*sinc((tap-center)/2),center1+phase/128',phaseQuantization='fraction>>9',response=response,sourceFirmwareSHA256=hashlib.sha256(raw).hexdigest()),indent=2)+'\n')

if __name__=='__main__':
    main()
