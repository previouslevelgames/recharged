#!/usr/bin/env python3
"""Owned AX compressor oracle: executes firmware 057B..006F on controlled inputs.

The selected-instruction interpreter follows native_ax_command_oracle.py and
adds only the compressor's own instructions (ABS/CMP/IFC/TST/JNZ/DEC/CLRP,
SRS/SI DMA registers and the SL extension). The DMA source is the original
__AXCompressorTable. No full DSP, kernel, ROM or game data is modelled.
"""
from pathlib import Path
import json, re, struct, sys

MASK = (1 << 40) - 1


def signed(x, bits):
    return (x & ((1 << bits) - 1)) - (1 << bits) if x & (1 << (bits - 1)) else x & ((1 << bits) - 1)


def sat16(x):
    return max(-32768, min(32767, x))


class Compressor:
    def __init__(self, words, main):
        self.words = words; self.main = main
        self.r = [0] * 32; self.r[8:12] = [65535] * 4; self.r[18] = 0xff
        self.a = [0, 0]; self.p = 0; self.mem = {}; self.mode40 = False; self.unsigned = False
        self.factor = 2; self.carry = False; self.zero = False
        self.pc = 0; self.loops = []; self.executed = 0; self.dma = []

    def read(self, r):
        if r in (28, 29): return self.a[r - 28] & 65535
        if r in (30, 31):
            x = signed(self.a[r - 30], 40)
            return (sat16(x // 65536) if self.mode40 else x >> 16) & 65535
        return self.r[r]

    def write(self, r, x):
        x &= 65535
        if r in (28, 29): self.a[r - 28] = (self.a[r - 28] & ~65535) | x
        elif r in (30, 31):
            k = r - 30
            self.a[k] = (signed(x, 16) * 65536) & MASK if self.mode40 else (self.a[k] & ~(65535 << 16)) | (x << 16)
        else: self.r[r] = x

    def acc(self, k, x): self.a[k] = x & MASK

    def advance(self, ar, d):
        assert self.r[8 + ar] == 65535
        self.r[ar] = (self.r[ar] + d) & 65535

    def store(self, address, value):
        self.mem[address] = value & 65535
        if address == 0xffcb:  # DSBL: an original main->DRAM transfer of `value` bytes
            assert self.mem[0xffc9] == 0, 'compressor DMA must be main-memory to DSP DRAM'
            source = (self.mem[0xffce] << 16) | self.mem[0xffcf]
            target = self.mem[0xffcd]
            assert value % 2 == 0 and source % 2 == 0
            for i in range(value // 2):
                self.mem[target + i] = self.main[source + 2 * i]
            self.dma.append((source, target, value))

    def run(self, start, stop):
        self.pc = start
        while self.pc != stop:
            assert self.executed < 4000, 'unbounded selected original routine'
            self.step()
        assert not self.loops

    def step(self):
        pc = self.pc; w = self.words[pc]; after = pc + 1; self.executed += 1
        old = [self.read(r) for r in range(32)]; oldar = self.r[:4]; ext = w & 255; pending = []
        # Extension side effects sample registers/memory before the primary.
        if w >> 12 >= 3 and w & 0xf700 not in (0x8200,) or w & 0xff00 == 0x8200:
            if ext & 0xce == 0x82:  # SL: store AC.M at AR0, load AXD.L/H from AR3
                pending.extend([('m', oldar[0], old[30 + (ext & 1)]), ('r', 24 + ((ext >> 4) & 3), self.mem[oldar[3]]),
                                ('ar', 0, 1), ('ar', 3, 1)])
            elif ext & 0xe4 == 0x20:  # S @ARn, ACx.L
                ar = ext & 3; pending.extend([('m', oldar[ar], old[28 + ((ext >> 3) & 3)]), ('ar', ar, 1)])
            elif ext & 0xc4 == 0x40:  # L reg, @ARn
                ar = ext & 3; pending.extend([('r', 24 + ((ext >> 3) & 7), self.mem[oldar[ar]]), ('ar', ar, 1)])
            elif ext == 0: pass
            else: raise AssertionError(f'unknown compressor extension {pc:04x}/{w:04x}')
        if w & 0xffe0 == 0x80: self.write(w & 31, self.words[pc + 1]); after += 1          # LRI
        elif w & 0xffe0 == 0xc0: self.write(w & 31, self.mem[self.words[pc + 1]]); after += 1  # LR
        elif w & 0xffe0 == 0xe0: self.mem[self.words[pc + 1]] = old[w & 31]; after += 1   # SR
        elif w & 0xfc00 == 0x1c00: self.write((w >> 5) & 31, old[w & 31])                # MRR
        elif w & 0xff80 == 0x1900:                                                         # LRRI
            ar = (w >> 5) & 3; self.write(w & 31, self.mem[oldar[ar]]); self.advance(ar, 1)
        elif w & 0xff00 == 0x1100:                                                         # BLOOPI
            count = w & 255; end = self.words[pc + 1]; after += 1
            if count: self.loops.append([after, end, count])
            else: after = end + 1
        elif w & 0xff00 == 0x1600:                                                         # SI
            self.store(0xff00 | (w & 0xff), self.words[pc + 1]); after += 1
        elif w & 0xf800 == 0x2800:                                                         # SRS
            self.store((self.r[18] << 8) | (w & 0xff), old[0x18 + ((w >> 8) & 7)])
        elif w == 0x02bf:                                                                  # CALL
            target = self.words[pc + 1]; after += 1
            assert target == 0x0084 and self.words[0x84:0x8a] == [0x26c9, 0x02a0, 0x0004, 0x029c, 0x0084, 0x02df]
            # Original DMA-busy poll; transfers above complete before it returns.
        elif w == 0x0294:                                                                  # JNZ
            after = self.words[pc + 1] if not self.zero else pc + 2
        elif w == 0x029f: after = self.words[pc + 1]                                       # JMP
        elif w == 0x0277:                                                                  # IFC
            if not self.carry: after = pc + 2
        elif w & 0xff00 in (0x8100, 0x8900): self.a[(w >> 11) & 1] = 0                     # CLR
        elif w & 0xff00 == 0x8200:                                                         # CMP
            x = signed(self.a[0], 40); y = signed(self.a[1], 40)
            result = signed(x - y, 40)
            self.carry = (x & MASK) >= (result & MASK); self.zero = result == 0
        elif w & 0xff00 == 0x8400: self.p = 0                                              # CLRP
        elif w & 0xff00 == 0x8a00: self.factor = 2                                         # M2
        elif w & 0xff00 == 0x8b00: self.factor = 1                                         # M0
        elif w & 0xff00 == 0x8d00: self.unsigned = True                                    # SET15
        elif w & 0xff00 == 0x8e00: self.mode40 = False                                     # SET16
        elif w & 0xff00 == 0x8f00: self.mode40 = True                                      # SET40
        elif w & 0xf700 == 0xa100:                                                         # ABS
            k = (w >> 11) & 1; self.acc(k, abs(signed(self.a[k], 40)))
        elif w & 0xf700 == 0xb100:                                                         # TST
            k = (w >> 11) & 1; self.zero = signed(self.a[k], 40) == 0
        elif w & 0xfe00 == 0x7a00:                                                         # DEC
            k = (w >> 8) & 1; self.acc(k, signed(self.a[k], 40) - 1)
        elif w & 0xfe00 == 0x4800:                                                         # ADDAX
            k = (w >> 8) & 1; ax = signed((old[26] << 16) | old[24], 32)
            self.acc(k, signed(self.a[k], 40) + ax)
        elif w & 0xff00 in (0x4e00, 0x4f00):                                               # ADDP
            k = (w >> 8) & 1; self.acc(k, signed(self.a[k], 40) + self.p)
        elif w & 0xe000 == 0xa000:                                                         # MULX family
            s = (w >> 12) & 1; t = (w >> 11) & 1; k = (w >> 8) & 1; op = (w >> 9) & 3
            x = old[24 + s * 2]; y = old[25 + t * 2]
            x = x if self.unsigned and not s else signed(x, 16)
            y = y if self.unsigned and not t else signed(y, 16)
            prior = self.p; self.p = x * y * self.factor
            if op == 2: self.acc(k, signed(self.a[k], 40) + prior)
            elif op == 3: self.acc(k, prior)
            elif op != 0: raise AssertionError('unqualified multiply variant')
        elif w & 0xfe00 == 0xf000: self.acc((w >> 8) & 1, self.a[(w >> 8) & 1] << 16)    # LSL16
        elif w & 0xf700 == 0x9100:                                                         # ASR16
            k = (w >> 11) & 1; self.acc(k, signed(self.a[k], 40) >> 16)
        else: raise AssertionError(f'unknown compressor primary {pc:04x}/{w:04x}')
        for kind, at, value in pending:
            if kind == 'r': self.write(at, value)
            elif kind == 'm': self.mem[at] = value
            else: self.advance(at, value)
        if self.loops and self.loops[-1][1] == pc:
            self.loops[-1][2] -= 1
            if self.loops[-1][2]: after = self.loops[-1][0]
            else: self.loops.pop()
        self.pc = after


if len(sys.argv) != 4:
    raise SystemExit('usage: native_ax_compressor_oracle.py PREPARED_DSP_CODE PREPARED_AXCOMP OUT_DIRECTORY')
code = Path(sys.argv[1]).read_text()
b = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})(?![0-9a-fA-F])', code)]
words = [b[i] * 256 + b[i + 1] for i in range(0, len(b), 2)]
assert len(words) == 4096
comp = Path(sys.argv[2]).read_text()
body = comp[comp.index('{', comp.index('__AXCompressorTable')) + 1:comp.index('};')]
table = [int(x) for x in re.findall(r'\b\d+\b', body)]
assert len(table) == 2016, 'original compressor table geometry differs'
TABLE_ADDRESS = 0x00321000
main = {}
for i, value in enumerate(table): main[TABLE_ADDRESS + 2 * i] = value
CL = 0x0e00


def run(counter, threshold, release, left, right):
    d = Compressor(words, main)
    for base, samples in ((0, left), (0xc0, right)):
        for i, x in enumerate(samples):
            d.mem[base + 2 * i] = (x >> 16) & 65535; d.mem[base + 2 * i + 1] = x & 65535
    # Literal pipelines load 98 ramp cells and 196 bus words per channel;
    # the two trailing ramp cells and right-pass bus words are never stored.
    for cell in (0x180, 0x181, 0x182, 0x183, 0xd68, 0xd69): d.mem[cell] = 0
    d.mem[0xce4] = counter
    for i, value in enumerate((threshold, release, TABLE_ADDRESS >> 16, TABLE_ADDRESS & 65535)):
        d.mem[CL + i] = value
    d.r[0] = CL
    d.run(0x057b, 0x006f)
    assert d.r[0] == CL + 4, 'compressor did not consume its four command words'
    out = [[signed((d.mem[base + 2 * i] << 16) | d.mem[base + 2 * i + 1], 32) for i in range(96)] for base in (0, 0xc0)]
    offset = (d.dma[0][0] - TABLE_ADDRESS) if d.dma else 0xffffffff
    return dict(counter=counter, threshold=threshold, release=release, left=left, right=right,
                after_counter=d.mem[0xce4], offset=offset, out_left=out[0], out_right=out[1], steps=d.executed)


cases = []
quiet = [[((i * 1777 + c * 311) % 50001) - 25000 for i in range(96)] for c in range(2)]
for counter in range(0, 11):
    cases.append(run(counter, 32768, 10, *quiet))                      # release or untouched
for counter in (0, 1, 5, 9, 10):
    loud = [list(quiet[0]), list(quiet[1])]
    loud[counter % 2][(counter * 7) % 96] = 32768 if counter % 3 else -32768
    cases.append(run(counter, 32768, 10, *loud))                       # attack from each level
edge = [[32767 if i % 2 else -32767 for i in range(96)] for _ in range(2)]
cases.append(run(0, 32768, 10, *edge))                                 # just below threshold
cases.append(run(3, 0, 10, *quiet))                                    # threshold 0 always attacks
cases.append(run(0, 65535, 3, *[[65534 - i for i in range(96)], [-65534 + i for i in range(96)]]))
cases.append(run(0, 32768, 3, *[[40000] * 96, [-40000] * 96]))         # custom release frames
extremes = [-2147483648, 2147483647, -1, 1, 0x00ffffff, -0x00ffffff, 0x12345678, -0x12345678]
cases.append(run(7, 32768, 10, *[[extremes[(i + c) % 8] for i in range(96)] for c in range(2)]))
for index in range(24):                                                # varied loud/quiet frames
    seed = index * 7919 + 13
    span = (index % 4 + 1) * 20000
    channels = [[((seed + i * (2027 + 64 * c)) % (2 * span + 1)) - span for i in range(96)] for c in range(2)]
    cases.append(run(index % 11, 32768, 10, *channels))

out = Path(sys.argv[3]); out.mkdir(parents=True, exist_ok=True)
wire = bytearray(b'AXCMP001') + struct.pack('<2016H', *table) + struct.pack('<I', len(cases))
for c in cases:
    wire += struct.pack('<HHH', c['counter'], c['threshold'], c['release'])
    wire += struct.pack('<96i', *c['left']) + struct.pack('<96i', *c['right'])
    wire += struct.pack('<HI', c['after_counter'], c['offset'] & 0xffffffff)
    wire += struct.pack('<96i', *c['out_left']) + struct.pack('<96i', *c['out_right'])
(out / 'compressor-oracle.bin').write_bytes(wire)
(out / 'compressor-oracle.json').write_text(json.dumps(dict(
    scope='owned057B..0609 compressor and 0084 DMA poll; controlled bus/counter inputs; original table; no full kernel',
    cases=cases), indent=1) + '\n')
attacks = sum(1 for c in cases if c['after_counter'] == c['release'] and c['offset'] != 0xffffffff and c['offset'] < 0x840)
print('Owned compressor oracle:', len(cases), 'cases,', attacks, 'attacks')
