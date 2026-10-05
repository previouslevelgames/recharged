"""Independent synthetic THP1.1 encoder; no retail data or codec dependency."""
import struct

def u32(*v): return struct.pack('>'+str(len(v))+'I',*v)
def segment(marker,data): return bytes([255,marker])+struct.pack('>H',len(data)+2)+data

def video(dc=0,width=16,height=16,overflow=False):
    table=bytes([2]+[0]*15)+bytes([0,16 if overflow else 4])
    q=segment(0xdb,bytes(1)+bytes([1]*64))
    sof=segment(0xc0,bytes([8])+struct.pack('>HH',height,width)+bytes([3,1,0x22,0,2,0x11,0,3,0x11,0]))
    huf=segment(0xc4,bytes([0])+table+bytes([16,1]+[0]*15+[0]))
    scan=segment(0xda,bytes([3,1,0,2,0,3,0,0,63,0]))
    blocks=((width+15)//16)*((height+15)//16)*6
    if overflow: bits=('1'*17+'0')*blocks
    else: bits=('1'+format(dc,'04b')+'0' if dc else '00')+'00'*(blocks-1)
    bits+='1'*((-len(bits))%8)
    packed=bytes(int(bits[n:n+8],2) for n in range(0,len(bits),8))
    return b'\xff\xd8'+q+sof+huf+scan+packed

def audio(mono=False,samples=14):
    # Independently known zero-predictor DSP: L repeating+1, R repeating-2.
    n=(samples+13)//14;left=(bytes([0])+bytes([0x11]*7))*n;right=(bytes([0])+bytes([0xee]*7))*n
    return u32(0 if mono else len(left),samples)+bytes(72)+left+(b'' if mono else right)

def movie(count=3,bad_second=False,silent=False,mono=False):
    bodies=[]
    for i in range(count):
        v=video(8 if i==0 else 0)
        if bad_second and i==1:v=v[:-2] # remove actual entropy; bounded decoder must fail.
        a=b'' if silent else audio(mono)
        raw=u32(0,0,len(v))+(b'' if silent else u32(len(a)))+v+a
        bodies.append(raw+bytes((-len(raw))%32))
    sizes=list(map(len,bodies));frames=[]
    for i,b in enumerate(bodies):frames.append(u32(sizes[(i+1)%count],sizes[(i-1)%count])+b[8:])
    types=bytes([0]) if silent else bytes([0,1]);meta=u32(len(types))+types+bytes(16-len(types))+u32(16,16,0)
    if not silent:meta+=u32(1 if mono else 2,32000,count*14,1)
    start=(48+len(meta)+31)&~31
    header=b'THP\0'+u32(0x11000,max(sizes),0 if silent else 14)+struct.pack('>f',30)+u32(count,sizes[0],sum(sizes),48,0,start,start+sum(sizes[:-1]))
    return header+meta+bytes(start-48-len(meta))+b''.join(frames)

def files():
    return {'Art/movies/test.thp':movie(),'Art/movies/bad.thp':movie(bad_second=True),'Art/movies/mono.thp':movie(mono=True),'Art/movies/silent.thp':movie(silent=True)}
