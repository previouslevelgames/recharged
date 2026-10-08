#!/usr/bin/env python3
"""Owned AX remote speaker oracle: executes firmware 02CC..0311, 03A2..0446 and
067C..06A9 on controlled inputs.

The selected-instruction interpreter follows native_ax_compressor_oracle.py and
adds only the instructions of these ranges, the 06AB LPF and the 0CB0/0CB3/0CD0
remote mixers. Entry modes are the firmware's own: 02CC runs under SET16 (0280
for the first voice, 046A after each voice); 03A2 inherits SET15 (0356), M2
(034C) and SET40 (0364), which every main mix routine and the biquad restore.
Multiplier low halves are unsigned under SET15, high halves signed; MUL is
signed. The bank-0 coefficients at DROM 0x1000 are an explicitly synthetic
conformance input, never retail. No full DSP, kernel, ROM or game data is
modelled. Instruction facts: Duddie's GameCube DSP hardware manual.
"""
from pathlib import Path
import json, re, struct, sys

MASK = (1 << 40) - 1
CL = 0x0e00
# 03FF..0446 order: main0, aux0, main1, aux1, main2, aux2, main3, aux3.
ACCUMULATORS = (0x0240, 0x0ac0, 0x0264, 0x0ae4, 0x0288, 0x0b08, 0x02ac, 0x0b2c)
PB_REMOTE = 0x033b   # DRAM word of PB byte 0xD6; 41 words through rmtIIR
PB_WORDS = 41


def signed(x, bits):
    return (x & ((1 << bits) - 1)) - (1 << bits) if x & (1 << (bits - 1)) else x & ((1 << bits) - 1)


def sat16(x):
    return max(-32768, min(32767, x))


class Remote:
    def __init__(self, words):
        self.words = words
        self.r = [0] * 32; self.r[8:12] = [65535] * 4; self.r[18] = 0xff
        self.a = [0, 0]; self.p = 0; self.mem = {}; self.main = {}
        self.mode40 = True; self.unsigned = False; self.factor = 2; self.zero = False
        self.pc = 0; self.loops = []; self.calls = []; self.executed = 0; self.dma = []

    def read(self, r):
        if r in (28, 29): return self.a[r - 28] & 65535
        if r in (30, 31):
            x = signed(self.a[r - 30], 40)
            return (sat16(x // 65536) if self.mode40 else x >> 16) & 65535
        return self.r[r]

    def write(self, r, x):
        x &= 65535
        if r in (28, 29): self.a[r - 28] = (self.a[r - 28] & ~65535 & MASK) | x
        elif r in (30, 31):
            k = r - 30
            # SXM: in 40-bit mode a middle write sign-extends and clears AC.L.
            self.a[k] = (signed(x, 16) * 65536) & MASK if self.mode40 else (self.a[k] & ~(65535 << 16) & MASK) | (x << 16)
        else:
            assert r < 16 or 24 <= r <= 27, f'unqualified register write {r}'
            self.r[r] = x

    def acc(self, k, x): self.a[k] = x & MASK

    def advance(self, ar, d):
        assert self.r[8 + ar] == 65535, 'remote ranges use linear addressing'
        self.r[ar] = (self.r[ar] + d) & 65535

    def operand(self, value, low):
        return value if self.unsigned and low else signed(value, 16)

    def store(self, address, value):
        self.mem[address] = value & 65535
        if address == 0xffcb:  # DSBL starts a DMA of `value` bytes
            direction = self.mem[0xffc9]
            main = (self.mem[0xffce] << 16) | self.mem[0xffcf]; dsp = self.mem[0xffcd]
            assert value % 2 == 0 and main % 2 == 0
            for i in range(value // 2):
                if direction: self.main[main + 2 * i] = self.mem[dsp + i]
                else: self.mem[dsp + i] = self.main[main + 2 * i]
            self.dma.append((direction, main, dsp, value))

    def run(self, start, stop):
        self.pc = start
        while self.pc != stop or self.calls:
            assert self.executed < 400000, 'unbounded selected original routine'
            self.step()
        assert not self.loops

    def step(self):
        pc = self.pc; w = self.words[pc]; after = pc + 1; self.executed += 1
        old = [self.read(r) for r in range(32)]; oldar = self.r[:4]; pending = []
        ext = (w & 0x7f) if w >> 12 == 3 else (w & 0xff) if w >> 12 > 3 else 0
        # Extension operands are sampled before the primary; writes follow it.
        if ext:
            if ext & 0xe4 == 0x20:                                   # S @ARd, $(1C+ss)
                ar = ext & 3; pending += [('m', oldar[ar], old[28 + ((ext >> 3) & 3)]), ('ar', ar, 1)]
            elif ext & 0xc0 == 0x40:                                 # L/LN $(18+ddd), @ARs
                ar = ext & 3; step = signed(self.r[4 + ar], 16) if ext & 4 else 1
                pending += [('r', 24 + ((ext >> 3) & 7), self.mem[oldar[ar]]), ('ar', ar, step)]
            elif ext & 0xc0 == 0x80:                                 # LS/SL(N)(M) $(18+dd), AC.M
                reg = 24 + ((ext >> 4) & 3); kind = (ext >> 1) & 7; value = old[30 + (ext & 1)]
                if kind & 1: pending += [('m', oldar[0], value), ('r', reg, self.mem[oldar[3]])]
                else: pending += [('r', reg, self.mem[oldar[0]]), ('m', oldar[3], value)]
                pending += [('ar', 0, signed(self.r[4], 16) if kind in (2, 3, 6, 7) else 1),
                            ('ar', 3, signed(self.r[7], 16) if kind in (4, 5, 6, 7) else 1)]
            elif ext & 0xcf == 0xc3:                                 # LDAX AXr, @ARs
                s = (ext >> 5) & 1; x = (ext >> 4) & 1
                pending += [('r', 26 + x, self.mem[oldar[s]]), ('r', 24 + x, self.mem[oldar[3]]),
                            ('ar', s, 1), ('ar', 3, 1)]
            elif ext & 0xcc == 0xc0:                                 # LD AX0.d, AX1.r, @ARs
                s = ext & 3
                pending += [('r', 24 + 2 * ((ext >> 5) & 1), self.mem[oldar[s]]),
                            ('r', 25 + 2 * ((ext >> 4) & 1), self.mem[oldar[3]]), ('ar', s, 1), ('ar', 3, 1)]
            else: raise AssertionError(f'unqualified remote extension {pc:04x}/{w:04x}')
        if w & 0xfffc == 0x0004: self.advance(w & 3, -1)                                   # DAR
        elif w & 0xfff0 == 0x0010: self.advance(w & 3, signed(self.r[4 + ((w >> 2) & 3)], 16))  # ADDARN
        elif w & 0xfefc == 0x0210: self.write(30 + ((w >> 8) & 1), self.words[self.r[w & 3]])  # ILRR
        elif w & 0xffe0 == 0x0080: self.write(w & 31, self.words[pc + 1]); after += 1          # LRI
        elif w & 0xffe0 == 0x00c0: self.write(w & 31, self.mem[self.words[pc + 1]]); after += 1  # LR
        elif w & 0xffe0 == 0x00e0: self.mem[self.words[pc + 1]] = old[w & 31]; after += 1      # SR
        elif w & 0xf800 == 0x0800: self.write(24 + ((w >> 8) & 7), signed(w & 255, 8))        # LRIS
        elif w == 0x0294: after = self.words[pc + 1] if not self.zero else pc + 2              # JNZ
        elif w == 0x0295: after = self.words[pc + 1] if self.zero else pc + 2                  # JZ
        elif w == 0x029f: after = self.words[pc + 1]                                           # JMP
        elif w == 0x02bf:                                                                      # CALL
            target = self.words[pc + 1]
            if target == 0x0084:
                # Original DMA-busy poll; transfers above complete before it returns.
                assert self.words[0x84:0x8a] == [0x26c9, 0x02a0, 0x0004, 0x029c, 0x0084, 0x02df]
                after = pc + 2
            else: self.calls.append(pc + 2); after = target
        elif w == 0x02df: after = self.calls.pop()                                             # RET
        elif w & 0xff00 == 0x1100:                                                             # BLOOPI
            count = w & 255; end = self.words[pc + 1]; after += 1
            if count: self.loops.append([after, end, count])
            else: after = end + 1
        elif w & 0xfe80 == 0x1400:                                                             # LSL/LSR #
            k = (w >> 8) & 1; imm = w & 0x3f
            if w & 0x40: self.acc(k, (self.a[k] & MASK) >> ((0x40 - imm) if imm else 0))
            else: self.acc(k, self.a[k] << imm)
        elif w & 0xff00 == 0x1600: self.store(0xff00 | (w & 0xff), self.words[pc + 1]); after += 1  # SI
        elif w & 0xff1f == 0x171f: self.calls.append(pc + 1); after = old[(w >> 5) & 7]       # CALLR
        elif w & 0xfe00 == 0x1800 or w & 0xfe00 == 0x1a00:                                     # LRR/SRR family
            ar = (w >> 5) & 3; mode = (w >> 7) & 3
            if w & 0x0200: self.mem[oldar[ar]] = old[w & 31]
            else: self.write(w & 31, self.mem[oldar[ar]])
            if mode: self.advance(ar, (0, -1, 1, signed(self.r[4 + ar], 16))[mode])
        elif w & 0xfc00 == 0x1c00: self.write((w >> 5) & 31, old[w & 31])                     # MRR
        elif w & 0xf800 == 0x2800: self.store((self.r[18] << 8) | (w & 0xff), old[24 + ((w >> 8) & 7)])  # SRS
        elif w & 0xfc00 == 0x3400:                                                             # ANDR
            k = (w >> 8) & 1; m = old[30 + k] & old[26 + ((w >> 9) & 1)]
            self.a[k] = (self.a[k] & ~(65535 << 16) & MASK) | (m << 16)
        elif w & 0xf800 == 0x4000:                                                             # ADDR
            k = (w >> 8) & 1; self.acc(k, signed(self.a[k], 40) + signed(old[24 + ((w >> 9) & 3)], 16) * 65536)
        elif w & 0xfc00 == 0x4800:                                                             # ADDAX
            k = (w >> 8) & 1; s = (w >> 9) & 1
            self.acc(k, signed(self.a[k], 40) + signed((old[26 + s] << 16) | old[24 + s], 32))
        elif w & 0xfe00 == 0x4c00:                                                             # ADD
            k = (w >> 8) & 1; self.acc(k, signed(self.a[k], 40) + signed(self.a[1 - k], 40))
        elif w & 0xfe00 == 0x4e00: k = (w >> 8) & 1; self.acc(k, signed(self.a[k], 40) + self.p)  # ADDP
        elif w & 0xfe00 == 0x6e00: self.acc((w >> 8) & 1, self.p)                              # MOVP
        elif w & 0xf700 == 0x8000: pass                                                        # NX
        elif w & 0xf700 == 0x8100: self.a[(w >> 11) & 1] = 0                                   # CLR
        elif w & 0xff00 == 0x8a00: self.factor = 2                                             # M2
        elif w & 0xff00 == 0x8b00: self.factor = 1                                             # M0
        elif w & 0xff00 == 0x8c00: self.unsigned = False                                       # CLR15
        elif w & 0xff00 == 0x8d00: self.unsigned = True                                        # SET15
        elif w & 0xff00 == 0x8e00: self.mode40 = False                                         # SET16
        elif w & 0xff00 == 0x8f00: self.mode40 = True                                          # SET40
        elif w & 0xf700 == 0x9100: k = (w >> 11) & 1; self.acc(k, signed(self.a[k], 40) >> 16)  # ASR16
        elif w & 0xf000 == 0x9000:                                                             # MUL family
            s = (w >> 11) & 1; k = (w >> 8) & 1; op = (w >> 9) & 3
            assert op or not w & 0x0100
            prior = self.p; self.p = signed(old[24 + s], 16) * signed(old[26 + s], 16) * self.factor
            if op == 2: self.acc(k, signed(self.a[k], 40) + prior)
            elif op == 3: self.acc(k, prior)
            elif op: raise AssertionError('unqualified multiply variant')
        elif w & 0xf700 == 0xb100: k = (w >> 11) & 1; self.zero = signed(self.a[k], 40) == 0  # TST
        elif w & 0xe000 == 0xa000:                                                             # MULX family
            s = (w >> 12) & 1; t = (w >> 11) & 1; k = (w >> 8) & 1; op = (w >> 9) & 3
            assert op or not w & 0x0100, f'unqualified MULX-space primary {pc:04x}/{w:04x}'
            prior = self.p
            self.p = self.operand(old[24 + s * 2], not s) * self.operand(old[25 + t * 2], not t) * self.factor
            if op == 2: self.acc(k, signed(self.a[k], 40) + prior)
            elif op == 3: self.acc(k, prior)
            elif op: raise AssertionError('unqualified multiply variant')
        elif w & 0xe700 == 0xc100:                                                             # CMPAXH
            k = (w >> 12) & 1
            self.zero = signed(signed(self.a[k], 40) - signed(old[26 + ((w >> 11) & 1)], 16) * 65536, 40) == 0
        elif w & 0xfc00 == 0xe000:                                                             # MADDX
            s = (w >> 9) & 1; t = (w >> 8) & 1
            self.p += self.operand(old[24 + s * 2], not s) * self.operand(old[25 + t * 2], not t) * self.factor
        elif w & 0xfe00 == 0xf000: self.acc((w >> 8) & 1, self.a[(w >> 8) & 1] << 16)        # LSL16
        else: raise AssertionError(f'unqualified remote primary {pc:04x}/{w:04x}')
        for kind, at, value in pending:
            if kind == 'r': self.write(at, value)
            elif kind == 'm': self.mem[at] = value & 65535
            else: self.advance(at, value)
        if self.loops and self.loops[-1][1] == pc:
            self.loops[-1][2] -= 1
            if self.loops[-1][2]: after = self.loops[-1][0]
            else: self.loops.pop()
        self.pc = after


if len(sys.argv) != 3:
    raise SystemExit('usage: native_ax_remote_oracle.py PREPARED_DSP_CODE OUT_DIRECTORY')
code = Path(sys.argv[1]).read_text()
b = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})(?![0-9a-fA-F])', code)]
words = [b[i] * 256 + b[i + 1] for i in range(0, len(b), 2)]
assert len(words) == 4096
assert words[0x0db3:0x0db7] == [0x0cb0, 0x0cb3, 0x0cd0, 0x0cd0], 'original remote handler table differs'
assert words[0x03d1:0x03d5] == [0x009b, 0x0005, 0x0099, 0x5555], 'original remote resampling step differs'

state = [0x2468ace1]


def rand(n):
    state[0] = (state[0] * 1103515245 + 12345) & 0xffffffff
    return (state[0] >> 8) % n


def rs16(): return rand(65536) - 32768


# Synthetic bank 0: extreme first rows exercise SRC saturation, the rest vary.
BANK = [32767, 32767, 32767, 32767, -32768, -32768, -32768, -32768, 32767, -32768, 32767, -32768]
BANK += [rs16() if i % 7 == 0 else rand(40001) - 20000 for i in range(512 - len(BANK))]


def voice(ctrl, mix, src_frac, history, iir, pcm, remote=1, dpop=None):
    pb = [remote, ctrl] + [x & 65535 for x in mix]
    pb += [(x & 65535) for x in (dpop or [0x1111 * (i + 1) for i in range(8)])]
    pb += [src_frac] + [x & 65535 for x in history]
    pb += [x & 65535 for x in iir] + [0] * (10 - len(iir))
    assert len(pb) == PB_WORDS
    return dict(pb=pb, pcm=pcm)


def run_case(accumulators, voices, addresses):
    d = Remote(words)
    for i, x in enumerate(BANK): d.mem[0x1000 + i] = x & 65535
    for base, values in zip(ACCUMULATORS, accumulators):
        for i, x in enumerate(values): d.mem[base + 2 * i] = (x >> 16) & 65535; d.mem[base + 2 * i + 1] = x & 65535
    d.mem[0x0ce1] = 0x0c60
    # The LPF's final source prefetch reads 0CC0; the mixers' final sample
    # prefetch reads 0CD2 (saved command pointer). Neither value is consumed.
    for cell in range(0x0cc0, 0x0cd2): d.mem[cell] = 0x5a5a
    d.mem[0x0cd2] = CL
    after = []
    handlers = []
    for v in voices:
        for i, x in enumerate(v['pb']): d.mem[PB_REMOTE + i] = x
        for i, x in enumerate(v['pcm']): d.mem[0x0c60 + i] = x & 65535
        d.mode40 = False
        d.a = [0x12_3456_789a, 0xfe_dcba_9876]  # stale accumulators; setup clears AC0 itself
        d.run(0x02cc, 0x0312)
        handlers.append([d.mem[0x0cd7 + i] for i in range(8)])
        d.mode40 = True; d.unsigned = True; d.factor = 2
        d.a = [0x7f_0000_1234, 0x80_1234_0000]; d.p = 0x55_5555_5555 - (1 << 40)
        d.r[24:28] = [0xbeef, 0xcafe, 0xdead, 0xf00d]
        d.run(0x03a2, 0x0447)
        after.append([d.mem[PB_REMOTE + i] for i in range(PB_WORDS)])
    final = [[signed((d.mem[base + 2 * i] << 16) | d.mem[base + 2 * i + 1], 32) for i in range(18)]
             for base in ACCUMULATORS]
    for i, address in enumerate(addresses):
        d.mem[CL + 2 * i] = address >> 16; d.mem[CL + 2 * i + 1] = address & 65535
    d.r[0] = CL
    d.run(0x067c, 0x006f)
    assert d.r[0] == CL + 8, 'remote output did not consume its four addresses'
    assert [x[:2] for x in d.dma] == [(1, a) for a in addresses]
    out = [[signed(d.main[a + 2 * i], 16) for i in range(18)] for a in addresses]
    return dict(initial=accumulators, voices=voices, after=after, handlers=handlers, final=final,
                output=out, steps=d.executed)


def pcm_pattern(kind):
    if kind == 0: return [((i * 1531) % 60001) - 30000 for i in range(96)]
    if kind == 1: return [32767 if i % 2 else -32768 for i in range(96)]
    if kind == 2: return [0] * 96
    if kind == 3: return [-32768] * 96
    return [rs16() for _ in range(96)]


ADDRESSES = [0x00500000, 0x00500024, 0x00500048, 0x0050006c]
zero = [[0] * 18 for _ in range(8)]
cases = []
mixes = [[0x8000, 0] * 8, [0xffff, 0xffff] * 8, [0x0001, 0x0001] * 8, [0x7fff, 0x8000] * 8]
for ctrl in (0x5555, 0xaaaa, 0xffff, 0x0000, 0x1b1b, 0xe4e4, 0x8001):                # handler selection
    for m in mixes:
        cases.append(run_case(zero, [voice(ctrl, m, 0, [0, 0, 0, 0], [], pcm_pattern(0))], ADDRESSES))
for frac in (0, 1, 0x5555, 0x8000, 0xffff):                                          # resampler phase/history
    cases.append(run_case(zero, [voice(0x5555, [0x8000, 0] * 8, frac, [32767, -32768, 1, -1], [],
                                       pcm_pattern(1))], ADDRESSES))
for iir in ([1, 0, 0x6a09, 0x15f6], [3, -1234, 0x2000, 0x5fff], [1, 32767, 0xffff, 0x8000],
            [0xffff, -32768, 1, 0xffff], [1, 0, 0, 0]):                              # remote LPF
    cases.append(run_case(zero, [voice(0x5555, [0x8000, 0] * 8, 0x1234, [5, 6, 7, 8], iir,
                                       pcm_pattern(0))], ADDRESSES))
cases.append(run_case(zero, [voice(0xffff, [0xffff, 0] * 8, 0, [0] * 4, [], pcm_pattern(3))], ADDRESSES))
edges = [0x007fffff, -0x00800000, 0x007ffff0, -0x007ffff0, 0x12345678, -0x12345678, 0x7fffffff, -0x80000000]
wide = [[edges[(c + i) % 8] for i in range(18)] for c in range(8)]                    # 24-bit wrap/output
cases.append(run_case(wide, [voice(0x5555, [0xffff, 0] * 8, 0, [0] * 4, [], pcm_pattern(1))], ADDRESSES))
cases.append(run_case(wide, [voice(0x0000, [0] * 16, 0, [0] * 4, [], pcm_pattern(0))], ADDRESSES))
for count in (2, 3, 4):                                                               # several voices
    vs = [voice(rand(65536), [rand(65536) for _ in range(16)], rand(65536), [rs16() for _ in range(4)],
                [1, rs16(), rand(65536), rand(65536)] if index % 2 else [], pcm_pattern(4),
                remote=0 if index == 1 else 1 + index) for index in range(count)]
    cases.append(run_case([[rs16() * 8 for _ in range(18)] for _ in range(8)], vs, ADDRESSES))
for _ in range(24):                                                                   # varied frames
    vs = [voice(rand(65536), [rand(65536) for _ in range(16)], rand(65536), [rs16() for _ in range(4)],
                [rand(4) and rand(65536) or 0, rs16(), rand(65536), rand(65536)] if rand(2) else [],
                pcm_pattern(rand(5))) for _ in range(1 + rand(3))]
    vs = [v if v['pb'][PB_WORDS - 10] != 2 else dict(v, pb=v['pb'][:PB_WORDS - 10] + [1] + v['pb'][PB_WORDS - 9:])
          for v in vs]  # selector 2 is the unsupported biquad
    cases.append(run_case([[rand(1 << 20) - (1 << 19) for _ in range(18)] for _ in range(8)], vs, ADDRESSES))

out = Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
wire = bytearray(b'AXRMT001') + struct.pack('<512h', *BANK) + struct.pack('<I', len(cases))
for c in cases:
    wire += struct.pack('<I', len(c['voices']))
    for values in c['initial']: wire += struct.pack('<18i', *values)
    for v, after in zip(c['voices'], c['after']):
        wire += struct.pack('<96h', *v['pcm']) + struct.pack('<41H', *v['pb']) + struct.pack('<41H', *after)
    for values in c['final']: wire += struct.pack('<18i', *values)
    for values in c['output']: wire += struct.pack('<18h', *values)
(out / 'remote-oracle.bin').write_bytes(wire)
(out / 'remote-oracle.json').write_text(json.dumps(dict(
    scope='owned 02CC..0311 handler table, 03A2..0446 remote branch with 06AB/0CB0/0CB3/0CD0, '
          '067C..06A9 output DMA; controlled PB/sample/accumulator inputs; synthetic bank 0; no full kernel',
    handlerTable=[hex(x) for x in words[0x0db3:0x0db7]],
    cases=[dict(handlers=[[hex(x) for x in h] for h in c['handlers']], steps=c['steps']) for c in cases]),
    indent=1) + '\n')
voices = sum(len(c['voices']) for c in cases)
print('Owned remote oracle:', len(cases), 'cases,', voices, 'voices')
