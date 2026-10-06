"""Original THP/NL/mixer diagnostic; generated media and actual consumed PCM."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import struct
import subprocess
import tempfile
from disc_fixture import write_disc
from test_original_movie_data import audio_oracle, video


def generated():
    frames, count = [], 96
    image = video(16, 16, 0)
    for frame in range(64):
        channels = []
        for channel in range(2):
            data = bytearray()
            for block in range((count + 13) // 14):
                data += bytes([frame % 12])
                for byte in range(7):
                    first = (frame + channel * 7 + block + byte * 2) % 16
                    second = (first + 1) % 16
                    data += bytes([first * 16 + second])
            channels.append(data)
        audio = struct.pack('>II', len(channels[0]), count) + bytes(72) + b''.join(channels)
        payload = struct.pack('>4I', 0, 0, len(image), len(audio)) + image + audio
        frames.append(payload + bytes(-len(payload) % 32))
    size = len(frames[0])
    assert all(len(frame) == size for frame in frames)
    frames = [struct.pack('>II', size, size) + frame[8:] for frame in frames]
    header = b'THP\0' + struct.pack('>11I', 0x11000, size, count,
        struct.unpack('>I', struct.pack('>f', 30.0))[0], len(frames), size,
        size * len(frames), 48, 0, 96, 96 + size * (len(frames) - 1))
    components = struct.pack('>I', 2) + bytes([0, 1]) + bytes([255]) * 14
    descriptors = struct.pack('>3I4I', 16, 16, 0, 2, 32000, count * len(frames), 1)
    return header + components + descriptors + b''.join(frames)


def reference(raw, limit=256):
    header = struct.unpack_from('>11I', raw, 4)
    comp = header[7]
    count = struct.unpack_from('>I', raw, comp)[0]
    types = list(raw[comp + 4:comp + 4 + count])
    cursor, size = header[9], header[5]
    samples = bytearray()
    for _ in range(min(header[4], limit)):
        sizes = struct.unpack_from('>' + 'I' * count, raw, cursor + 8)
        payload = cursor + 8 + count * 4
        for component, component_size in zip(types, sizes):
            if component == 1:
                samples += audio_oracle(raw[payload:payload + component_size], 0)
            payload += component_size
        next_size = struct.unpack_from('>I', raw, cursor)[0]
        cursor += size
        size = next_size
    return bytes(samples)


def verify(capture, original, expect_paced=False):
    expected = reference(original)
    blocks = {}
    for index in range(len(expected) // 384):
        blocks.setdefault(expected[index * 384:(index + 1) * 384], []).append(index)
    assert len(capture) % 384 == 0, 'DMA capture contains a partial hardware block'
    indices, zeros, unknown = [], 0, []
    for index in range(len(capture) // 384):
        block = capture[index * 384:(index + 1) * 384]
        if block == bytes(384):
            zeros += 1
        elif block in blocks:
            indices.append(blocks[block][0])
        else:
            unknown.append(index)
    assert indices and not unknown, 'Consumed PCM differs from independent original ADPCM/mix arithmetic'
    strict_sequence = None
    if expect_paced:
        raw_blocks = [capture[n:n + 384] for n in range(0, len(capture), 384)]
        first = next(n for n, block in enumerate(raw_blocks) if block != bytes(384))
        last = max(n for n, block in enumerate(raw_blocks) if block != bytes(384))
        played = b''.join(raw_blocks[first:last + 1])
        strict_sequence = played == expected[:len(played)]
        assert strict_sequence, 'Timed DMA repeated/skipped/reordered original source PCM'
    return {'strict_contiguous_source_prefix': strict_sequence, 'captured_blocks': len(capture) // 384, 'silent_blocks': zeros,
            'matched_nonzero_blocks': len(indices), 'unknown_blocks': unknown,
            'distinct_source_blocks': len(set(indices)),
            'source_block_indices': indices,
            'consecutive_repeats': sum(a == b for a, b in zip(indices, indices[1:])),
            'capture_sha256': hashlib.sha256(capture).hexdigest(),
            'reference_sha256': hashlib.sha256(expected).hexdigest()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable', type=Path)
    parser.add_argument('module', type=Path)
    parser.add_argument('--owned', type=Path)
    parser.add_argument('--raw-oracle', type=Path)
    parser.add_argument('--movie', default='art/movies/credits.thp')
    parser.add_argument('--milliseconds', type=int, default=400)
    parser.add_argument('--evidence', type=Path)
    parser.add_argument('--expect-paced', action='store_true',
                        help='Require contiguous original PCM at the source-selected DMA rate')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='charged-original-movie-audio-') as folder:
        folder = Path(folder)
        raw = args.raw_oracle.read_bytes() if args.raw_oracle else generated()
        disc = args.owned.resolve() if args.owned else folder / 'synthetic.iso'
        if not args.owned:
            write_disc(disc, files={'art/movies/credits.thp': raw}, partition_size=0x10000)
        capture = folder / 'consumed.pcm'
        command = [str(args.executable.resolve()), str(args.module.resolve()),
                   str(disc), args.movie, str(args.milliseconds), str(capture)]
        result = subprocess.run(command, cwd=folder, capture_output=True, text=True,
            timeout=30, env={**os.environ, 'SDL_VIDEODRIVER': 'dummy',
                            'SDL_RENDER_DRIVER': 'software', 'SDL_AUDIODRIVER': 'dummy'})
        evidence = {'command': command, 'exit': result.returncode,
                    'stdout': result.stdout, 'stderr': result.stderr}
        if result.returncode == 0:
            if not args.owned or args.raw_oracle:
                evidence['oracle'] = verify(capture.read_bytes(), raw, args.expect_paced)
            evidence['scope'] = 'Original audio-only THP mode0; no AX/mode1/video/main/CRT, arbitrary CPU preemption or reset-to-first-IRQ cycle acceptance'
            if args.expect_paced:
                summary = next(json.loads(line) for line in result.stdout.splitlines() if line.startswith('{'))
                nominal = args.milliseconds * summary['input_rate'] / 96000
                # Source starts before preload and SDL retains a device-period tail.
                # Rate acceptance is broad here; native_ai_timing verifies exact cells.
                assert abs(summary['callbacks'] - nominal) <= max(8, nominal * 0.05), 'Native DMA callback rate differs from source96-frame period'
        if args.evidence:
            args.evidence.write_text(json.dumps(evidence, indent=2) + '\n')
        print(result.stdout + result.stderr)
        assert result.returncode == 0, 'Original movie audio source execution failed'
        print('Independent original PCM oracle passed' + (' with contiguous timed DMA source sequence.' if args.expect_paced else '.'))


if __name__ == '__main__':
    main()
