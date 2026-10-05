#!/usr/bin/env python3
"""Independent binary32/FMA/order oracle for original weighted software pose."""
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile

from test_weighted_skin import chunks, decoded


def fraction(word):
    sign, exponent, mantissa = word >> 31, (word >> 23) & 255, word & 0x7fffff
    assert exponent != 255
    if exponent:
        mantissa += 1 << 23
        power = exponent - 150
    else:
        power = -149
    value = Fraction(mantissa << power, 1) if power >= 0 else Fraction(mantissa, 1 << -power)
    return -value if sign else value


def rounded(value, negative_zero=False):
    """Round an exact rational to binary32 nearest-even, without host fma."""
    if not value:
        return 0x80000000 if negative_zero else 0
    sign = 0x80000000 if value < 0 else 0
    n, d = abs(value.numerator), value.denominator
    exponent = n.bit_length() - d.bit_length()
    if (n < d << exponent) if exponent >= 0 else (n << -exponent < d):
        exponent -= 1
    shift = 149 if exponent < -126 else 23 - exponent
    numerator, denominator = (n << shift, d) if shift >= 0 else (n, d << -shift)
    mantissa, remainder = divmod(numerator, denominator)
    if remainder * 2 > denominator or (remainder * 2 == denominator and mantissa & 1):
        mantissa += 1
    if exponent < -126:
        assert mantissa <= 1 << 23
        return sign | mantissa
    if mantissa == 1 << 24:
        exponent += 1
        mantissa >>= 1
    assert exponent <= 127
    return sign | ((exponent + 127) << 23) | (mantissa - (1 << 23))


def multiply(a, b):
    return rounded(fraction(a) * fraction(b), bool((a ^ b) >> 31))


def add(a, b):
    return rounded(fraction(a) + fraction(b), a == b == 0x80000000)


def madd(a, b, c):
    product = fraction(a) * fraction(b)
    negative = not product and bool((a ^ b) >> 31) and c == 0x80000000
    return rounded(product + fraction(c), negative)


def bits(value):
    return struct.unpack(">I", struct.pack(">f", value))[0]


def software(matrix, vector, weight, previous, position=True, fused=True):
    result = []
    for component in range(3):
        value = multiply(matrix[component], vector[0])
        for coordinate in (1, 2):
            coefficient = matrix[coordinate*4+component]
            value = madd(coefficient, vector[coordinate], value) if fused else add(multiply(coefficient, vector[coordinate]), value)
        if position:
            value = add(value, matrix[12+component])
        result.append(madd(value, weight, previous[component]) if fused else add(multiply(value,weight), previous[component]))
    return result


def product(a, b):
    # Existing selected PSMTX44Concat's explicitly rounded left grouping.
    # SoftwareSkinModel below then follows its distinct fused instruction order.
    result = []
    for row in range(4):
        for col in range(4):
            values = [multiply(a[row*4+k], b[k*4+col]) for k in range(4)]
            result.append(add(add(add(values[0], values[1]), values[2]), values[3]))
    return result


def arithmetic(executable, directory):
    rng = random.Random(0x135)
    cases = []
    witness = [bits(0)] * 29
    witness[0], witness[4] = 0xbf800002, 0x3f800001
    witness[15], witness[16], witness[17] = bits(1), bits(1), 0x3f800001
    witness[19], witness[20], witness[22] = bits(1), 0x3f800001, bits(1)
    cases.append(witness)
    accumulation = [bits(0)] * 29
    accumulation[0], accumulation[15], accumulation[16], accumulation[19] = 0x3f800001, bits(1), bits(1), bits(1)
    accumulation[22], accumulation[23], accumulation[26] = 0x3f7ffffe, bits(-1), bits(-1)
    cases.append(accumulation)
    for _ in range(1200):
        values = [bits(rng.uniform(-2,2) * 2 ** rng.randrange(-18,19)) for _ in range(29)]
        values[3] = values[7] = values[11] = bits(0)
        values[15] = bits(1)
        values[22] = bits(rng.choice([0, .125, .5, .75, 1, rng.random()]))
        cases.append(values)
    path = directory/'equations.bin'
    path.write_bytes(struct.pack(">I", len(cases)) + b"".join(struct.pack(">29I", *case) for case in cases))
    actual = json.loads(subprocess.check_output([executable, '--equations', str(path)], text=True, timeout=15))
    expected, witnesses = [], 0
    for values in cases:
        result = software(values[:16], values[16:19], values[22], values[23:26])
        result += software(values[:16], values[19:22], values[22], values[26:29], False)
        expected.append(result)
        nonfused = software(values[:16], values[16:19], values[22], values[23:26], fused=False)
        nonfused += software(values[:16], values[19:22], values[22], values[26:29], False, fused=False)
        witnesses += result != nonfused
    assert actual == expected, 'Scalar equation bits differ from independent exact rational rounding'
    assert expected[0][0] == bits(2 ** -46) and expected[1][0] == bits(-2 ** -46)
    assert witnesses >= 2, 'Arithmetic oracle did not distinguish explicit fused operations'
    return {'binary32_equation_cases': len(cases), 'fused_difference_witnesses': witnesses}


def verify(executable, model_path, hierarchy_path, seed):
    raw = model_path.read_bytes()
    model = decoded(raw)
    actual = json.loads(subprocess.check_output([executable, '--inspect', str(model_path), str(hierarchy_path), str(seed)], text=True, timeout=30))
    for inverse, pose, matrix in zip(actual['inverse'], actual['input'], actual['matrices'], strict=True):
        assert matrix == product(inverse, pose), 'Original inverse-bind/global-pose matrix order differs'
    ids = None
    hierarchy = hierarchy_path.read_bytes()
    root = list(chunks(hierarchy, 0, len(hierarchy)))
    assert len(root) == 1 and root[0][0] == 0x80018000
    for kind, begin, end in chunks(hierarchy, root[0][1], root[0][2]):
        if kind == 0x18003:
            ids = list(struct.unpack_from('>'+'I'*((end-begin)//4), hierarchy, begin))
    assert ids is not None
    order_witnesses, vertices = 0, 0
    for source, output, mapping in zip(model['packets'], actual['packets'], actual['node_maps'], strict=True):
        assert mapping == [ids.index(bone) for bone in source['bones']]
        assert output['palette'] == [[actual['matrices'][node][c*4+r] for r in range(3) for c in range(4)] for node in mapping]
        expected_positions, expected_normals = [], []
        for vertex in source['vertices']:
            bones, weights = list(vertex[12:16]), list(vertex[16:20])
            largest = max(range(4), key=lambda lane: fraction(weights[lane]))
            bones[0], bones[largest] = bones[largest], bones[0]
            weights[0], weights[largest] = weights[largest], weights[0]
            lanes = []
            for bone, weight in zip(bones, weights):
                if not fraction(weight):
                    break
                lanes.append((bone, weight))
            position = normal = [bits(0)] * 3
            for bone, weight in sorted(lanes, key=lambda lane: lane[0]):
                matrix = actual['matrices'][mapping[bone]]
                position = software(matrix, vertex[0:3], weight, position)
                normal = software(matrix, vertex[3:6], weight, normal, False)
            lane_position = [bits(0)] * 3
            for bone, weight in lanes:
                lane_position = software(actual['matrices'][mapping[bone]], vertex[0:3], weight, lane_position)
            order_witnesses += position != lane_position
            expected_positions.append(position)
            expected_normals.append(normal)
        assert output['positions'] == expected_positions, 'Weighted position grouping/order/translation differs'
        assert output['normals'] == expected_normals, 'Weighted direct3x3 normal bits differ'
        vertices += len(source['vertices'])
    assert model_path.read_bytes() == raw, 'Pose modified source game-data bytes'
    return {'seed':seed,'sha256':hashlib.sha256(raw).hexdigest(),'vertices':vertices,
            'order_difference_witnesses':order_witnesses,'fingerprint':actual['fingerprint']}


def main():
    executable = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='charged-weighted-pose-') as temporary:
        directory = Path(temporary)
        result = arithmetic(executable, directory)
        if len(sys.argv) == 4:
            model, hierarchy = Path(sys.argv[2]), Path(sys.argv[3])
            result['owned_poses'] = [verify(executable, model, hierarchy, seed) for seed in range(8)]
            assert sum(p['order_difference_witnesses'] for p in result['owned_poses']) > 0
        else:
            assert len(sys.argv) == 2
            subprocess.run([executable], check=True, timeout=15)
            model, hierarchy = directory/'model.rlg', directory/'model.shier'
            subprocess.run([executable,'--fixture',str(model),str(hierarchy)],check=True,timeout=10)
            baseline = model.read_bytes()
            result['generated_poses'] = [verify(executable,model,hierarchy,seed) for seed in range(24)]
            # Deliberately nonunit authored sums demonstrate no weight or
            # normal renormalization in the actual retained software consumer.
            modified = bytearray(baseline)
            for packet in decoded(baseline)['packets']:
                for vertex in range(packet['unique']):
                    for lane in range(4):
                        modified[packet['bone_offset']+vertex*4+lane]=lane
                        struct.pack_into('>f',modified,packet['weight_offset']+vertex*16+lane*4,.5)
            model.write_bytes(modified)
            result['nonunit_poses'] = [verify(executable,model,hierarchy,seed) for seed in range(8)]
            assert sum(p['order_difference_witnesses'] for p in result['generated_poses']+result['nonunit_poses']) > 0
        print(json.dumps(result,indent=2))


if __name__ == '__main__':
    main()
