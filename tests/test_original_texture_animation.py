#!/usr/bin/env python3
"""Independent raw Wii record and original single-step arithmetic fixtures."""
import argparse
import json
import math
from pathlib import Path
import re
import struct
import subprocess


def float_bits(value):
    try:
        return struct.unpack(">I", struct.pack(">f", value))[0]
    except OverflowError:
        return 0xFF800000 if value < 0 else 0x7F800000


def float_value(bits):
    return struct.unpack(">f", struct.pack(">I", bits))[0]


def expected_steps(mode, count, direction, paused, initial, durations, deltas):
    """Evaluate state transitions on fixture integers, rounding each add to f32."""
    frame = 0
    time = initial
    result = []
    for delta in deltas:
        if paused == 0 and count >= 2:
            time = float_bits(float_value(time) + float_value(delta))
            if float_value(time) >= float_value(durations[frame]):
                time = 0
                if mode == 0:
                    frame = (frame + 1) % count
                elif mode == 1:
                    if direction > 0:
                        frame += 1
                        if frame >= count:
                            frame -= 2
                            direction = -1
                    else:
                        frame -= 1
                        if frame < 0:
                            frame = 1
                            direction = 1
                elif mode == 2:
                    frame = min(frame + 1, count - 1)
        result += [delta, frame & 0xFFFFFFFF, direction & 0xFFFFFFFF, time]
    return result


def generate():
    records = []
    durations = [float_bits(v) for v in (0.25, 0.125, 0.75, 0.0)]
    deltas = [float_bits(v) for v in (0.0, 0.125, 8.0, 0.0, -0.5, 0.25, 1.0, 1.0)]
    for mode in range(3):
        for count in (1, 2, 4):
            for pause in (0, 1, 3):
                for direction in (-1, 1):
                    name = f"mode{mode}-frames{count}-paused{pause}-dir{direction}"
                    header = struct.pack(">IIiIiBBBBIfI", 0x5F6C6669, 0xE19A2F43,
                        count, mode, direction, pause, 0xE1, 0x6D, 0x97,
                        0xFFFFFFFF, -0.0, 0xFEDCBA98)
                    frames = b"".join(struct.pack(">II", 0x93441A00+i, durations[i])
                                      for i in range(count))
                    words = [36, 1, 0x5F6C6669, 0xE19A2F43, count, mode,
                             direction & 0xFFFFFFFF, pause, 0xE1, 0x6D, 0x97,
                             0xFFFFFFFF, 0x80000000, 0xFEDCBA98]
                    for i in range(count):
                        words += [0x93441A00+i, durations[i]]
                    words += [len(deltas)] + expected_steps(mode, count, direction,
                                pause, 0x80000000, durations[:count], deltas)
                    records.append((name, header+frames, words))
    # NaN, infinity, negative durations/times are authored float transport and
    # original update inputs. No native normalization or validity policy applies.
    for label, duration, initial, delta in (
        ("negative", float_bits(-1.0), float_bits(-4.0), float_bits(-0.125)),
        ("nan-time", float_bits(0.25), 0x7FC12345, float_bits(1.0)),
        ("nan-delta", float_bits(0.25), float_bits(0.0), 0x7FCFEDCB),
        ("infinity", float_bits(0.25), float_bits(0.0), 0x7F800000),
        ("nan-duration", 0x7FCABC01, float_bits(0.0), float_bits(0.5)),
    ):
        header = struct.pack(">IIiIiBBBBIII", 0x5F6C6669, 0xFFFFFFFF, 2, 0,
                             1, 0, 0xFF, 0, 0xAA, 0xA5A5A5A5, initial, 0)
        frames = struct.pack(">IIII", 0xFFFFFFFF, duration, 0x80000000, 0x80000000)
        words = [36, 1, 0x5F6C6669, 0xFFFFFFFF, 2, 0, 1, 0, 0xFF, 0, 0xAA,
                 0xA5A5A5A5, initial, 0, 0xFFFFFFFF, duration, 0x80000000, 0x80000000, 1]
        words += expected_steps(0, 2, 1, 0, initial, [duration, 0x80000000], [delta])
        records.append((label, header+frames, words))
    for size in (0, 4, 32, 35):
        records.append((f"short{size}", bytes.fromhex("5f6c6669")+bytes(40), [size,0]))
    records.append(("wrong-magic", bytes.fromhex("69666c5f")+bytes(40), [36,0]))
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    results = []
    for name, raw, words in generate():
        data_path = args.directory/(name+".bin")
        oracle_path = args.directory/(name+".oracle")
        data_path.write_bytes(raw)
        oracle_path.write_text(" ".join(map(str,words))+"\n")
        command = [str(args.binary.resolve()),str(data_path.resolve()),str(oracle_path.resolve())]
        completed = subprocess.run(command, capture_output=True, text=True, timeout=30)
        output = completed.stdout+completed.stderr
        if completed.returncode or "runtime error:" in output or "Sanitizer" in output:
            raise AssertionError(f"{name}: exit {completed.returncode}\n{output}")
        match = re.search(r"checks=(\d+)",output)
        if not match:
            raise AssertionError(f"{name}: no original source result\n{output}")
        results.append({"name":name,"checks":int(match[1]),"cmd":command,"output":output})
    report = {"scope":"Bounded transport/queue/original manager/update qualifier. Source loading, pool vtables, inventory ownership, whole original font callbacks and GPU readiness are not linked/executed by this partial-link test.",
              "records":results,"count":len(results),"checks":sum(v["checks"] for v in results)}
    (args.directory/"results.json").write_text(json.dumps(report,indent=2)+"\n")
    print(f"{report['count']} independent Wii/original animation fixtures; {report['checks']} checks")


if __name__ == "__main__":
    main()
