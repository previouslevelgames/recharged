"""Independent THP wire/codec oracles; never initialize a substitute movie manager."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
import tempfile


def words(*values):
    return struct.pack('>' + 'I' * len(values), *values)


def put_metadata(folder, raw, expected):
    assert len(raw) == 160 and len(expected) == 51
    folder.mkdir(parents=True)
    (folder / 'metadata.bin').write_bytes(raw)
    (folder / 'metadata.txt').write_text(' '.join(f'{value:08x}' for value in expected) + '\n')


def segment(marker, payload):
    return bytes([255, marker]) + struct.pack('>H', len(payload) + 2) + payload


def video(width, height, dc):
    # Quantization=1 and DC=0/8 produce exact constant 128/129 luma.
    table = bytes([2] + [0] * 15) + bytes([0, 4])
    quant = segment(0xdb, bytes(1) + bytes([1] * 64))
    frame = segment(0xc0, bytes([8]) + struct.pack('>HH', height, width) + bytes([3, 1, 0x22, 0, 2, 0x11, 0, 3, 0x11, 0]))
    huffman = segment(0xc4, bytes([0]) + table + bytes([16, 1] + [0] * 15 + [0]))
    scan = segment(0xda, bytes([3, 1, 0, 2, 0, 3, 0, 0, 63, 0]))
    blocks = ((width + 15) // 16) * ((height + 15) // 16) * 6
    bits = ('1' + format(dc, '04b') + '0' if dc else '00') + '00' * (blocks - 1)
    bits += '1' * (-len(bits) % 8)
    entropy = bytes(int(bits[p:p + 8], 2) for p in range(0, len(bits), 8))
    return b'\xff\xd8' + quant + frame + huffman + scan + entropy


def quarter_video(dc, ac):
    # Four luma blocks each contain DC and coefficient (0,1) only. Chroma is DC0.
    # This selects the retail SDK's _quarterIDCT path, including its store order.
    size = abs(ac).bit_length()
    amplitude = ac if ac > 0 else ac + (1 << size) - 1
    quant = segment(0xdb, bytes(1) + bytes([1] * 64))
    frame = segment(0xc0, bytes([8]) + struct.pack('>HH', 16, 16) +
                    bytes([3, 1, 0x22, 0, 2, 0x11, 0, 3, 0x11, 0]))
    huffman = segment(0xc4, bytes([0, 2] + [0] * 15 + [0, 4]) +
                      bytes([16, 2] + [0] * 15 + [0, size]))
    scan = segment(0xda, bytes([3, 1, 0, 2, 0, 3, 0, 0, 63, 0]))
    first_dc = '11000' if dc == 8 else '0'
    coefficient = '1' + format(amplitude, f'0{size}b') + '0'
    bits = first_dc + coefficient + ('0' + coefficient) * 3 + '00' * 2
    bits += '1' * (-len(bits) % 8)
    entropy = bytes(int(bits[p:p + 8], 2) for p in range(0, len(bits), 8))
    return b'\xff\xd8' + quant + frame + huffman + scan + entropy


def audio_oracle(encoded, layout):
    offset, count = struct.unpack_from('>II', encoded)
    channels = []
    for right in (offset != 0, False):
        start, coeff_offset, hist_offset = (80 + offset, 40, 76) if right else (80, 8, 72)
        coefficients = struct.unpack_from('>16h', encoded, coeff_offset)
        history1, history2 = struct.unpack_from('>hh', encoded, hist_offset)
        samples = []
        for sample in range(count):
            block, within = start + (sample // 14) * 8, sample % 14
            descriptor = encoded[block]
            nibble = (encoded[block + 1 + within // 2] >> (4 if within % 2 == 0 else 0)) & 15
            if nibble >= 8:
                nibble -= 16
            predictor = (descriptor >> 4) & 7
            accumulator = nibble * (1 << (descriptor & 15)) * 2048 + coefficients[predictor * 2] * history1 + coefficients[predictor * 2 + 1] * history2
            rounded = max(-(1 << 31), min((1 << 31) - 1, accumulator * 32 + 32768))
            result = rounded // 65536
            samples.append(result)
            history2, history1 = history1, result
        channels.append(samples)
    samples = [value for pair in zip(*channels) for value in pair] if layout == 0 else channels[0] + channels[1]
    return struct.pack('<' + 'h' * len(samples), *samples)


def synthetic(folder):
    rng = random.Random(0x544850154)
    float_bits = [0, 0x80000000, 0x3f800000, 0x41f00000, 0x7f800000, 0xff800000, 0x7fc12345, 1]
    for index in range(64):
        header = [rng.getrandbits(32) for _ in range(11)]
        header[3] = float_bits[index % len(float_bits)]
        count = index % 17
        types = [rng.randrange(256) for _ in range(16)]
        sizes = [rng.getrandbits(32) for _ in range(7)]
        frame = [0, 1, 0x7fffffff, 0x80000000, 0xffffffff] + [rng.getrandbits(32) for _ in range(11)]
        magic = bytes(rng.randrange(256) for _ in range(4))
        raw = magic + words(*header) + words(count) + bytes(types) + words(*sizes) + words(*frame)
        put_metadata(folder / f'metadata-{index:02}', raw, header + [count] + types + sizes + frame)

    for index, (width, height, dc) in enumerate(((16, 16, 0), (16, 16, 8), (32, 16, 8), (16, 32, 0), (32, 32, 8))):
        case = folder / f'video-{index:02}'
        case.mkdir()
        (case / 'video.bin').write_bytes(video(width, height, dc))
        (case / 'video.txt').write_text(f'{width:x} {height:x} {128 + dc // 8:x} 80 80\n')

    # Literal pixel rows follow the paired-single stores in both original
    # RVL THPDec.c _quarterIDCT routines: tmp7, tmp8, swapped tmp6, swapped tmp5.
    # These are independent source-instruction oracles, not decoder snapshots.
    quarter_rows = (
        (0, 64, [139, 137, 134, 125, 130, 121, 118, 116]),
        (8, 64, [140, 138, 135, 126, 131, 122, 119, 117]),
        (0, -64, [116, 118, 121, 130, 125, 134, 137, 139]),
        (8, -64, [117, 119, 122, 131, 126, 135, 138, 140]),
        (8, 4096, [255, 255, 255, 0, 255, 0, 0, 0]),
    )
    for index, (dc, ac, row) in enumerate(quarter_rows):
        case = folder / f'video-quarter-{index:02}'
        case.mkdir()
        (case / 'video.bin').write_bytes(quarter_video(dc, ac))
        (case / 'video.txt').write_text('10 10 ffffffff 80 80\n')
        (case / 'video-y.bin').write_bytes(bytes(row * 32))

    for index, count in enumerate((1, 13, 14, 15, 28, 39, 56, 96)):
        for mono in (False, True):
            channel_bytes = ((count + 13) // 14) * 8
            coeffs = [rng.randrange(-4096, 4097) for _ in range(32)]
            hist = [rng.randrange(-32768, 32768) for _ in range(4)]
            channels = []
            for channel in range(1 if mono else 2):
                output = bytearray()
                for block in range(channel_bytes // 8):
                    output += bytes([(block % 8 << 4) | ((index + block) % 16)])
                    output += bytes(rng.randrange(256) for _ in range(7))
                channels.append(output)
            encoded = words(0 if mono else channel_bytes, count) + struct.pack('>32h4h', *coeffs, *hist) + b''.join(channels)
            case = folder / f'audio-{index:02}-{int(mono)}'
            case.mkdir()
            (case / 'audio.bin').write_bytes(encoded)
            for layout in (0, 1):
                (case / ('audio-interleaved.pcm' if layout == 0 else 'audio-planar.pcm')).write_bytes(audio_oracle(encoded, layout))


def owned(folder, path, name):
    encoded = path.read_bytes()
    header = list(struct.unpack_from('>11I', encoded, 4))
    comp_offset = header[7]
    component_count = struct.unpack_from('>I', encoded, comp_offset)[0]
    types = list(encoded[comp_offset + 4:comp_offset + 20])
    current, first_size, frame_count = header[9], header[5], header[4]
    cursor = comp_offset + 20
    video_info, audio_info = [0, 0, 0], [0, 0, 0, 0]
    for component in types[:component_count]:
        if component == 0:
            video_info = list(struct.unpack_from('>3I', encoded, cursor))
            cursor += 12
        elif component == 1:
            audio_info = list(struct.unpack_from('>4I', encoded, cursor))
            cursor += 16
        else:
            raise AssertionError('unsupported actual component type')
    selected, size, total_samples = [], first_size, 0
    for frame_index in range(frame_count):
        frame_words = list(struct.unpack_from('>16I', encoded, current))
        component_sizes = list(struct.unpack_from('>' + 'I' * component_count, encoded, current + 8))
        assert current + size <= len(encoded)
        payload = current + 8 + 4 * component_count
        audio_component = None
        for component, component_size in zip(types, component_sizes):
            if component == 1:
                audio_component = encoded[payload:payload + component_size]
                total_samples += struct.unpack_from('>I', audio_component, 4)[0]
            if frame_index in (0, frame_count // 2, frame_count - 1):
                case = folder / f'owned-{name}-{frame_index:05}-{component}'
                case.mkdir()
                if component == 0:
                    (case / 'video.bin').write_bytes(encoded[payload:payload + component_size])
                    (case / 'video.txt').write_text(f'{video_info[0]:x} {video_info[1]:x} ffffffff 0 0\n')
                else:
                    (case / 'audio.bin').write_bytes(audio_component)
                    for layout in (0, 1):
                        (case / ('audio-interleaved.pcm' if layout == 0 else 'audio-planar.pcm')).write_bytes(audio_oracle(audio_component, layout))
            payload += component_size
        assert payload <= current + size and current + size - payload < 32
        if frame_index in (0, frame_count // 2, frame_count - 1):
            raw = encoded[:48] + words(component_count) + bytes(types) + words(*video_info, *audio_info, *frame_words)
            put_metadata(folder / f'owned-{name}-{frame_index:05}-metadata', raw, header + [component_count] + types + video_info + audio_info + frame_words)
            selected.append({'frame': frame_index, 'offset': current, 'size': size, 'component_sizes': component_sizes})
        next_size = frame_words[0]
        current += size
        size = next_size
    assert current == len(encoded) and size == first_size
    assert not audio_info[2] or total_samples == audio_info[2]
    return {'path': str(path), 'bytes': len(encoded), 'sha256': hashlib.sha256(encoded).hexdigest(), 'frames': frame_count, 'audio_samples': total_samples, 'selected': selected}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable')
    parser.add_argument('--fixtures', type=Path)
    parser.add_argument('--owned', type=Path, help='private directory containing Art/movies from an owned image')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='charged-original-movie-data-') as temporary:
        folder = args.fixtures or Path(temporary)
        folder.mkdir(parents=True, exist_ok=True)
        synthetic(folder)
        audit = []
        if args.owned:
            for name in ('nlgintrowide', 'credits'):
                audit.append(owned(folder, args.owned / 'Art/movies' / (name + '.thp'), name))
        (folder / 'oracle-manifest.json').write_text(json.dumps({'scope': 'wire records and actual CPU decoder only; no original movie readiness or GPU fidelity claim', 'owned': audit}, indent=2) + '\n')
        result = subprocess.run([args.executable, str(folder)], check=False)
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
