"""Check actual canonical GX light-register bytes against the original Wii SDK."""
import argparse
import math
from pathlib import Path
import struct
import subprocess
import tempfile

def check(path):
    raw = Path(path).read_bytes()
    if len(raw) != 576:
        raise AssertionError("Six genuine 96-byte GX light packets required")
    directions = ((1,0,0),(-1,0,0),(0,1,0),(0,-1,0),(0,0,1),(0,0,-1))
    slots = (0,1,2,3,6,7)
    f32 = lambda v: struct.unpack('>f', struct.pack('>f', v))[0]
    bits = lambda v: struct.unpack('>I', struct.pack('>f', v))[0]
    # Owned retail 803A4DB8 loads DD5E0B6B from 806E73BC and multiplies
    # the three submitted directions. No host-private GX object layout used.
    distance = struct.unpack('>f', bytes.fromhex('dd5e0b6b'))[0]
    checks = 1
    for i, (x,y,z) in enumerate(directions):
        packet = raw[i*96:(i+1)*96]
        words = struct.unpack_from('>17I', packet, 1)
        if packet[0] != 0x10 or words[0] != (0xF << 16) | (0x600 + 0x10*slots[i]):
            raise AssertionError("Actual light XF register selection/count changed")
        if list(words[1:4]) != [0,0,0] or words[4] != 0x12345678+i:
            raise AssertionError("Padding or big-endian colour changed")
        if list(words[5:11]) != [bits(0),bits(0),bits(1),bits(0),bits(0),bits(1)]:
            raise AssertionError("Submitted attenuation changed")
        if list(words[11:14]) != [bits(f32(distance*float(v))) for v in (x,y,z)]:
            raise AssertionError("Native GX specular distance does not match original Wii SDK/retail")
        hx,hy,hz = -float(x),-float(y),f32(1.0+-float(z))
        mag = f32(f32(f32(hx*hx)+f32(hy*hy))+f32(hz*hz))
        inverse = f32(1/f32(math.sqrt(mag))) if mag else mag
        if list(words[14:17]) != [bits(f32(v*inverse)) for v in (hx,hy,hz)]:
            raise AssertionError("Original half-direction or zero-vector quirk changed")
        if packet[69:] != bytes(27):
            raise AssertionError("Real display-list tail padding changed")
        checks += 45
    return checks

if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--executable',required=True)
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mscharged-wii-lighting-') as directory:
        capture=Path(directory)/'lights.xf'
        result=subprocess.run([args.executable,str(capture)],capture_output=True,text=True,timeout=20)
        print(result.stdout,end='')
        print(result.stderr,end='')
        if result.returncode:
            raise SystemExit(result.returncode)
        print(f"Original Wii GX lighting: {check(capture)} register/byte checks passed. Generated hardware descriptors only; no game lighting or Draw acceptance.")
