#!/usr/bin/env python3
"""Annotate <module>+0x<offset> references in a diagnostic report with symbols.

The rollback's comparisons (RECHARGED_ROLLBACK_TEST_COMPARE) print addresses
inside the game module as offsets. This adds the symbol containing each one, read with nm
from the module's own symbol table (Release modules keep it; RelWithDebInfo also gives lines with
addr2line). Usage:

    python3 tools/symbolize_offsets.py build/relwithdebinfo/recharged_game_us.so report.txt

With --group (for a comparison summary, <file>.summary), the entries of each
section are merged by symbol: the tests of one global at several offsets add
up to one line, most frequent first.
"""
from __future__ import annotations

import bisect
import re
import subprocess
import sys
from pathlib import Path


def load_symbols(module: Path):
    output = subprocess.run(["nm", "-C", "-S", "-n", "--defined-only", str(module)],
                            check=True, capture_output=True, text=True).stdout
    starts, entries = [], []
    for line in output.splitlines():
        parts = line.split(" ", 3)
        if len(parts) == 4:
            address, size, _kind, name = parts
            size = int(size, 16)
        elif len(parts) == 3:
            address, _kind, name = parts
            size = 0
        else:
            continue
        starts.append(int(address, 16))
        entries.append((int(address, 16), size, name))
    return starts, entries


def describe(offset: int, starts, entries) -> str:
    index = bisect.bisect_right(starts, offset) - 1
    while index >= 0:
        start, size, name = entries[index]
        if size == 0 or offset < start + size:
            return f"{name}+0x{offset - start:x}"
        index -= 1
        if start + size <= offset and size:
            break
    return "?"


def group(lines, pattern, starts, entries) -> None:
    entry = re.compile(r"^\s*(\d+) tests \((\d+)\.\.(\d+)\), (\d+) bytes: (.*)$")
    section, merged = None, {}

    def flush():
        for name, (tests, first, last, size, count) in sorted(merged.items(), key=lambda item: -item[1][0]):
            print(f"{tests:5d} tests ({first}..{last}), {size} bytes, {count} entries: {name}")
        merged.clear()

    for line in lines:
        if line.startswith("== "):
            flush()
            section = line
            print("\n" + line)
            continue
        match = entry.match(line)
        if not section or not match:
            if not section:
                print(line)
            continue
        tests, first, last, size, rest = int(match[1]), int(match[2]), int(match[3]), int(match[4]), match[5]
        found = pattern.findall(rest)
        if "allocation" in rest:
            # Class (first word) and call site, each offset named by its symbol.
            name = pattern.sub(lambda hit: re.sub(r"\+0x[0-9a-f]+$", "",
                                                  describe(int(hit[1], 16), starts, entries)), rest)
            name = re.sub(r", first differing \+0x[0-9a-f]+", "", name)
        elif found:
            name = re.sub(r"\+0x[0-9a-f]+$", "", describe(int(found[-1], 16), starts, entries))
        else:
            name = re.sub(r", first differing \+0x[0-9a-f]+", "", rest)
        old = merged.get(name, (0, first, last, 0, 0))
        # Tests of different offsets of one symbol may overlap: the sum is an upper bound.
        merged[name] = (old[0] + tests, min(old[1], first), max(old[2], last), old[3] + size, old[4] + 1)
    flush()


def main() -> int:
    arguments = [argument for argument in sys.argv[1:] if argument != "--group"]
    if len(arguments) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    module, report = Path(arguments[0]), Path(arguments[1])
    starts, entries = load_symbols(module)
    pattern = re.compile(re.escape(module.name) + r"\+0x([0-9a-f]+)")
    lines = report.read_text(errors="replace").splitlines()
    if "--group" in sys.argv[1:]:
        group(lines, pattern, starts, entries)
        return 0
    for line in lines:
        found = pattern.findall(line)
        if found:
            line += "   <- " + ", ".join(describe(int(hex_offset, 16), starts, entries) for hex_offset in found)
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
