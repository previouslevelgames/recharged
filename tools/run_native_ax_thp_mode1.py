"""Bounded whole AX + original THPSimple(1) audio lifecycle qualifier."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import struct
import shutil
import subprocess
import sys
import tempfile

tests = Path(__file__).resolve().parents[1] / 'tests'
sys.path.insert(0, str(tests))
from disc_fixture import write_disc
from test_original_movie_audio import generated, verify

def media():
    original = generated()
    size = struct.unpack_from('>I', original, 24)[0]
    count = 1024
    raw = bytearray(original[:96] + original[96:] * (count // 64))
    struct.pack_into('>I', raw, 20, count)
    struct.pack_into('>I', raw, 28, size * count)
    struct.pack_into('>I', raw, 44, 96 + size * (count - 1))
    struct.pack_into('>I', raw, 88, count * 96)
    return bytes(raw)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('ax_image', type=Path)
    parser.add_argument('os_image', type=Path)
    parser.add_argument('thp_image', type=Path)
    parser.add_argument('--owned', type=Path)
    parser.add_argument('--raw-oracle', type=Path)
    parser.add_argument('--movie', default='art/movies/credits.thp')
    parser.add_argument('--milliseconds', type=int, default=400)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    if args.owned and not args.raw_oracle:
        parser.error('owned execution requires the private extracted movie oracle')
    with tempfile.TemporaryDirectory(prefix='charged-ax-thp-mode1-') as folder:
        folder = Path(folder)
        raw = args.raw_oracle.read_bytes() if args.raw_oracle else media()
        disc = args.owned.resolve() if args.owned else folder / 'synthetic.iso'
        if not args.owned:
            write_disc(disc, files={args.movie: raw}, partition_size=0x80000)
        command = [str(args.executable.resolve())]
        for image in (args.ax_image, args.os_image):
            command += [str(image.resolve()), hashlib.sha256(image.read_bytes()).hexdigest()]
        capture = folder / 'consumed.pcm'
        command += [str(args.thp_image.resolve()), str(disc), args.movie,
                    str(capture), str(args.milliseconds)]
        result = subprocess.run(command, cwd=folder, capture_output=True, text=True,
            timeout=35, env={**os.environ, 'SDL_VIDEODRIVER': 'dummy',
                            'SDL_RENDER_DRIVER': 'software', 'SDL_AUDIODRIVER': 'dummy'})
        evidence = {'command': command, 'exit': result.returncode,
                    'stdout': result.stdout, 'stderr': result.stderr}
        print(result.stdout + result.stderr, flush=True)
        failure = None
        try:
            assert result.returncode == 0, 'Original combined AX/THP mode1 source execution failed'
            evidence['oracle'] = verify(capture.read_bytes(), raw, True)
            summary = next(json.loads(line) for line in result.stdout.splitlines()
                           if line.startswith('{'))
            evidence['timing'] = summary
            nominal = args.milliseconds * 32000 / 96000
            assert abs(summary['mode1_ax_callbacks'] - nominal) <= max(12, nominal * .08), \
                'Combined original AX/THP DMA cadence differs from source96-frame periods'
        except Exception as error:
            failure = error
            evidence['failure'] = str(error)
            if capture.exists():
                # Preserve the strong contiguous-prefix failure. Arithmetic-only
                # evidence diagnoses it; it never replaces the acceptance oracle.
                try:
                    evidence['arithmetic_only_diagnostic'] = verify(capture.read_bytes(), raw, False)
                except Exception as arithmetic_error:
                    evidence['arithmetic_failure'] = str(arithmetic_error)
            target = (args.evidence.resolve().parent / (args.evidence.stem + '-failure')
                      if args.evidence else Path.cwd() / 'native_ax_thp_mode1-failure')
            target.mkdir(parents=True, exist_ok=True)
            if capture.exists():
                shutil.copyfile(capture, target / 'consumed.pcm')
                evidence['preserved_pcm'] = str(target / 'consumed.pcm')
            (target / 'evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')
            print(f'Preserved failing AX/THP evidence: {target}', file=sys.stderr, flush=True)
        evidence['scope'] = ('Whole original AXInit and THPSimple(1), genuine predecessor/rings/NL/PCM; '
            'explicit generated banks and audio-only video omission; no authentic ROM, '
            'GameAudio cue, MovieInit/renderer/main or original full CRT teardown acceptance; '
            'real-time owner fixture yields without altering source3ms DMA clock or callback decisions')
        if args.evidence:
            args.evidence.write_text(json.dumps(evidence, indent=2) + '\n')
        if failure:
            raise failure
        print('Consumed mode1 PCM matches the independent contiguous source oracle.', flush=True)

if __name__ == '__main__':
    main()
