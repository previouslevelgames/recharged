#!/usr/bin/env python3
"""Actual source word/studio methods compared with independently authored Wii fields."""

import argparse
from pathlib import Path
import random
import struct
import subprocess
import tempfile


def invoke(binary, mode, path):
    result = subprocess.run([str(binary), mode, str(path)], capture_output=True, text=True, timeout=20)
    if result.returncode or result.stderr:
        raise AssertionError(f"{mode}: {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result.stdout.splitlines()


def qualify_addresses(binary, directory):
    rng = random.Random(0x166ADD)
    cases = []
    for format in (0, 10, 25, 1, 2, 255, 65535):
        for flags in (0, 0xffffffff, 0x00200001, 0x80000000):
            for variant in range(8):
                fields = [rng.randrange(65536) for _ in range(8)]
                fields[1] = format
                if variant == 0:
                    fields = [0, format, 0, 0, 65535, 65535, 0x1234, 0xabcd]
                cases.append((flags, fields))
    payload = bytearray()
    expected = []
    address_flags = sum(1 << bit for bit in (11, 12, 13, 14))
    for index, (flags, fields) in enumerate(cases):
        authored = struct.pack(">8H", *fields)
        payload += struct.pack(">I", flags) + authored
        pb = bytearray([0xa5] * 320)
        pb[0x6e:0x7e] = authored
        if fields[1] in (10, 25):
            # Literal retail field oracle:32 coefficient bytes zeroed, gain
            # occupies the high u16, predictor and history remain zero.
            gain = 0x0800 if fields[1] == 10 else 0x0100
            pb[0x7e:0xa6] = struct.pack(">16H4H", *([0] * 16), gain, 0, 0, 0)
        flags = (flags & ~address_flags) | 0x8400
        expected.append(f"address {index} {(struct.pack('>I', flags) + pb).hex()}")
    path = directory / "authored-address-words.bin"
    path.write_bytes(payload)
    actual = invoke(binary, "address", path)
    assert actual[:-1] == expected, "Actual source address/sync/ADPCM fields differ from retail word oracle"
    assert actual[-1] == f"address-qualified {len(cases)}"
    return len(cases), len(cases) * 324


def qualify_studio(binary, directory):
    rng = random.Random(0x166570)
    totals = [0] * 20
    # PB depop memory order differs from original studio order. The remote
    # studio memory interleaves Main/Aux despite source print calls grouping them.
    pb_to_studio = (0, 4, 8, 1, 5, 9, 2, 6, 10, 3, 7, 11, 12, 16, 13, 17, 14, 18, 15, 19)
    payload = bytearray()
    expected = []

    def reset():
        payload.append(0)
        totals[:] = [0] * 20

    def add(fields):
        assert len(fields) == 20
        payload.append(1)
        payload.extend(struct.pack(">20h", *fields))
        for index, value in enumerate(fields):
            totals[index] += value

    def print_frame():
        payload.append(2)
        pairs = []
        for index, total in enumerate(totals):
            samples = 96 if index < 12 else 18
            magnitude = abs(total) // samples
            if magnitude == 0:
                value, delta, remaining = 0, 0, 0
            else:
                slope = min(20, magnitude) * (-1 if total < 0 else 1)
                value, delta, remaining = total, -slope, total - slope * samples
            pairs.append((value, delta))
            totals[index] = remaining
        wire = b"".join(struct.pack(">ih", *pairs[index]) for index in pb_to_studio)
        expected.append(f"studio {len(expected)} {wire.hex()}")

    print_frame()  # Original zero BSS/init output.
    for average in (-22, -21, -20, -2, -1, 0, 1, 2, 20, 21, 22):
        for residual in (-1, 0, 1):
            reset()
            add([(96 if i < 12 else 18) * average + residual for i in range(20)])
            print_frame()
            print_frame()  # Actual residual progression/reset behavior.
    reset()
    # Multiple voices accumulate before printing, then fade without new voices.
    for _ in range(64):
        add([rng.randrange(-32768, 32768) for _ in range(20)])
    for _ in range(48):
        print_frame()
    reset()
    for _ in range(48):
        add([rng.randrange(-32768, 32768) for _ in range(20)])
        if rng.randrange(3) == 0:
            add([rng.randrange(-32768, 32768) for _ in range(20)])
        print_frame()
    path = directory / "authored-studio-operations.bin"
    path.write_bytes(payload)
    actual = invoke(binary, "studio", path)
    assert actual[:-1] == expected, "Whole original PrintStudio word/delta/update order differs from independent oracle"
    assert actual[-1] == f"studio-qualified {len(expected)}"
    return len(expected), len(expected) * 120


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="charged-original-ax-words-") as temporary:
        directory = Path(temporary)
        address_cases, address_bytes = qualify_addresses(arguments.binary.resolve(), directory)
        studio_frames, studio_bytes = qualify_studio(arguments.binary.resolve(), directory)
    print(f"Original AX words passed: {address_cases} actual AXSetVoiceAddr cases/{address_bytes} independent bytes; "
          f"{studio_frames} whole source studio frames/{studio_bytes} independent packed bytes. "
          "Original source flags, gain/predictor hi/lo, arithmetic and residual order retained; "
          "no AXInit/DSP/THPmode1/audio readiness.")


if __name__ == "__main__":
    main()
