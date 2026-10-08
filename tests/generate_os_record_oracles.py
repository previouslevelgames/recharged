from pathlib import Path
import argparse, random, struct

parser=argparse.ArgumentParser()
parser.add_argument("output",type=Path)
args=parser.parse_args()
args.output.parent.mkdir(parents=True,exist_ok=True)
rng=random.Random(0x517)
states=[]
plays=[]
def checksum(data):
    return sum(struct.unpack('>'+str((len(data)-4)//4)+'I',data[4:]))&0xffffffff
for n in range(12):
    payload=bytes(rng.randrange(256) for _ in range(28))
    if n==0:payload=bytes([0x80,2,3,4])+bytes(24)
    wire=struct.pack('>I',0)+payload
    wire=struct.pack('>I',checksum(wire))+payload
    states.append(wire)
for n in range(20):
    title=[rng.randrange(65536) for _ in range(40)]
    title[:5]=[0,1,0x80,0x7fff,0xffff]
    times=[0,1,0xffffffffffffffff,0x8000000000000000,0x7fffffffffffffff,
           0x0123456789abcdef,0xfedcba9876543210]
    start=times[n%len(times)] if n<len(times) else rng.getrandbits(64)
    stop=times[(n+3)%len(times)] if n<len(times) else rng.getrandbits(64)
    wire=struct.pack('>I40H',0,*title)+bytes(rng.randrange(256) for _ in range(4))
    wire+=struct.pack('>QQ',start,stop)+b'R4QE01'+bytes(rng.randrange(256) for _ in range(18))
    assert len(wire)==128
    wire=struct.pack('>I',checksum(wire))+wire[4:]
    plays.append(wire)
lines=['#pragma once','#include <array>','#include <cstdint>']
for name,records,size in [('State',states,32),('Play',plays,128)]:
    lines.append(f'constexpr std::array<std::array<unsigned char,{size}>,{len(records)}> {name}Oracle = {{{{')
    for wire in records:lines.append('{{'+','.join(str(b) for b in wire)+'}},')
    lines.append('}};')
    lines.append(f'constexpr std::array<std::uint32_t,{len(records)}> {name}Checksums = {{'+','.join(hex(checksum(w))+'u' for w in records)+'};')
args.output.write_text("\n".join(lines)+"\n")
