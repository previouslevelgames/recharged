#!/usr/bin/env python3
"""Bounded original AX CPU source oracle; no DSP, AXInit, or audio readiness."""

from __future__ import annotations

import argparse
import random
import subprocess
import tempfile
from pathlib import Path


def run(binary: Path, mode: str, path: Path | None = None) -> list[str]:
    arguments = [str(binary), mode]
    if path is not None:
        arguments.append(str(path))
    result = subprocess.run(arguments, text=True, capture_output=True, timeout=20)
    if result.returncode or result.stderr:
        raise AssertionError(f"{arguments}: {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result.stdout.splitlines()


def depop_oracle(binary: Path, directory: Path) -> int:
    # Independent mathematical oracle uses signed integer division with explicit
    # truncation toward zero, including clamp and zero-residual boundaries.
    cases = []
    random_source = random.Random(0x162D)
    for frame in (96, 18):
        values = {0, -(1 << 31), (1 << 31) - 1}
        for average in (-22, -21, -20, -2, -1, 0, 1, 2, 20, 21, 22):
            for residual in (-1, 0, 1):
                values.add(average * frame + residual)
        values.update(random_source.randrange(-(1 << 31), 1 << 31) for _ in range(256))
        cases.extend((frame, value) for value in sorted(values))
    fixture = directory / "depop.txt"
    fixture.write_text("".join(f"{frame} {value}\n" for frame, value in cases))
    actual = run(binary, "depop", fixture)
    expected = []
    for index, (frame, value) in enumerate(cases):
        magnitude = abs(value) // frame
        average = -magnitude if value < 0 else magnitude
        if magnitude:
            slope = max(-20, min(20, average))
            expected.append(f"depop {index} {value - slope * frame} {value} {-slope}")
        else:
            expected.append(f"depop {index} 0 0 0")
    assert actual[:-1] == expected, "Source arithmetic differs from the independent integer oracle"
    assert actual[-1] == f"depop-qualified {len(cases)}"
    return len(cases) * 3


def voice_oracle(binary: Path, directory: Path) -> tuple[int, int]:
    # Python lists represent the authored priority policy without native linked
    # list pointers: free voices LIFO, steal the oldest lowest eligible priority,
    # callbacks LIFO. No implementation source is parsed to derive these results.
    queues = {priority: [] for priority in range(32)}
    queues[0] = list(reversed(range(8)))
    voices = [dict(priority=0, context=0, state=1, depop=0, sync=0, callback=False)
              for _ in range(8)]
    pending = []
    commands = []
    expected = []
    callback_count = 0
    default_sync = sum(1 << bit for bit in (2, 5, 19, 21, 23, 27, 28))

    def callback(operation: int, index: int) -> None:
        nonlocal callback_count
        voice = voices[index]
        if voice["callback"]:
            callback_count += 1
            expected.append(f"callback {operation} {index} {voice['priority']} "
                            f"{voice['context']} {voice['state']} {voice['depop']}")

    def defaults(voice: dict) -> None:
        voice["state"] = 0
        voice["sync"] = default_sync

    def submit(command: str, *arguments: int) -> None:
        commands.append(" ".join(map(str, (command, *arguments))))
        operation = len(commands)
        if command == "acquire":
            priority, context = arguments
            index = queues[0].pop(0) if queues[0] else None
            if index is None:
                for candidate_priority in range(1, priority):
                    if queues[candidate_priority]:
                        index = queues[candidate_priority].pop()
                        if voices[index]["state"] == 1:
                            voices[index]["depop"] = 1
                        callback(operation, index)
                        break
            if index is not None:
                voice = voices[index]
                queues[priority].insert(0, index)
                voice.update(priority=priority, context=context, callback=True)
                defaults(voice)
            expected.append(f"acquire {operation} {-1 if index is None else index}")
        elif command == "free":
            index, = arguments
            voice = voices[index]
            queues[voice["priority"]].remove(index)
            if voice["state"] == 1:
                voice["depop"] = 1
            defaults(voice)
            voice["priority"] = 0
            queues[0].insert(0, index)
        elif command == "priority":
            index, priority = arguments
            voice = voices[index]
            queues[voice["priority"]].remove(index)
            voice["priority"] = priority
            queues[priority].insert(0, index)
        elif command == "state":
            index, state = arguments
            voice = voices[index]
            if voice["state"] != state:
                voice["state"] = state
                voice["sync"] |= 1 << 2
                if state == 0:
                    voice["depop"] = 1
        elif command == "callback":
            index, = arguments
            pending.insert(0, index)
        elif command == "service":
            while pending:
                index = pending.pop(0)
                voice = voices[index]
                if voice["priority"] > 0:
                    callback(operation, index)
                    queues[voice["priority"]].remove(index)
                    queues[0].insert(0, index)
                    # Original callback servicing does not call SetPBDefault.
                    voice["priority"] = 0
        else:
            raise AssertionError(command)
        for index, voice in enumerate(voices):
            expected.append(f"voice {operation} {index} {voice['priority']} {voice['context']} "
                            f"{voice['state']} {voice['depop']} {voice['sync']}")
        for priority, queue in queues.items():
            suffix = "" if not queue else " " + " ".join(map(str, queue))
            expected.append(f"queue {operation} {priority}{suffix}")

    context = 0x1020304050607000
    for priority in (1, 1, 4, 5, 2, 2, 7, 8):
        submit("acquire", priority, context)
        context += 1
    for index in range(8):
        submit("state", index, 1)
    submit("acquire", 1, context)  # No same-priority stealing.
    context += 1
    submit("acquire", 2, context)  # Steals the oldest priority1 before priority2.
    context += 1
    submit("callback", 0)
    submit("callback", 1)
    submit("callback", 2)
    submit("service")  # Actual original callback order and retained PB state.
    submit("callback", 0)
    submit("service")  # Already-free voice callback is skipped by original source.

    random_source = random.Random(0x162A)
    for _ in range(192):
        active = [i for i, voice in enumerate(voices) if voice["priority"]]
        option = random_source.randrange(5)
        if option == 0 or not active:
            submit("acquire", random_source.randrange(1, 32), context)
            context += 1
        elif option == 1:
            submit("free", random_source.choice(active))
        elif option == 2:
            submit("priority", random_source.choice(active), random_source.randrange(1, 32))
        elif option == 3:
            submit("state", random_source.choice(active), random_source.randrange(2))
        else:
            selected = random_source.sample(active, min(len(active), random_source.randrange(1, 4)))
            for index in selected:
                submit("callback", index)
            submit("service")
    fixture = directory / "voice-operations.txt"
    fixture.write_text("\n".join(commands) + "\n")
    actual = run(binary, "voices", fixture)
    assert actual[:-1] == expected, "Source voice/context/callback trace differs from priority oracle"
    assert actual[-1].startswith("voices-qualified ")
    return len(expected), callback_count


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="charged-original-ax-") as temporary:
        directory = Path(temporary)
        arithmetic_checks = depop_oracle(arguments.binary.resolve(), directory)
        trace_checks, callbacks = voice_oracle(arguments.binary.resolve(), directory)
        auxiliary = run(arguments.binary.resolve(), "aux")
        assert len(auxiliary) == 1 and auxiliary[0].startswith("aux-qualified ")
        auxiliary_checks = int(auxiliary[0].split()[1])
    print(f"Original AX CPU qualification passed: {arithmetic_checks} independent arithmetic values, "
          f"{trace_checks} ordered voice/list/context records ({callbacks} genuine source callbacks), "
          f"{auxiliary_checks} source auxiliary/layout/lifetime checks. "
          "No AXInit, DSP completion, MovieInit(1), device output, or full audio readiness claimed.")


if __name__ == "__main__":
    main()
