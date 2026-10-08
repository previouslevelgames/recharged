#!/usr/bin/env python3
"""Reject unresolved game vtable/VTT data in an original-source module."""

import argparse
import re
import subprocess
import sys
from pathlib import Path


def unresolved_tables(output: str, object_format: str) -> tuple[list[str], int]:
    missing = []
    runtime_tables = 0
    for number, line in enumerate(output.splitlines(), 1):
        symbol = line.strip()
        if not symbol:
            continue
        # nm -u -j emits names only on GNU, LLVM, and Apple nm. Do not
        # silently accept an error message or a different output format.
        if not re.fullmatch(r"[A-Za-z_.$][A-Za-z0-9_.$@+\-]*", symbol):
            raise ValueError(f"Malformed nm names-only output at line {number}: {line!r}")
        name = symbol.split("@", 1)[0]
        if object_format == "macho" and name.startswith("_"):
            name = name[1:]  # Darwin C symbol prefix, not the Itanium prefix.
        if not name.startswith(("_ZTV", "_ZTT")):
            continue
        owner = name[4:]
        # St is the Itanium std:: substitution. N starts a nested name.
        # Match the exact namespace spelling, not a class-name substring:
        # e.g. StadiumLight and __cxxabiv1Lookalike must still fail.
        std_owner = re.match(r"(?:St|NSt)[1-9][0-9]*[A-Za-z_]", owner)
        abi_owner = re.match(r"N(?:10__cxxabiv1|3std)[1-9][0-9]*[A-Za-z_]", owner)
        if std_owner or abi_owner:
            runtime_tables += 1
        else:
            missing.append(symbol)
    return sorted(set(missing)), runtime_tables


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--object-format", choices=("elf", "macho"), required=True)
    args = parser.parse_args()
    try:
        if not args.module.is_file():
            raise ValueError(f"Original module does not exist: {args.module}")
        command = [args.nm, "-u", "-j", str(args.module)]
        result = subprocess.run(command, text=True, capture_output=True, check=False)
        if result.returncode != 0:
            raise ValueError(f"nm failed ({result.returncode}): {result.stderr.strip()}")
        missing, runtime = unresolved_tables(result.stdout, args.object_format)
    except (OSError, ValueError) as error:
        print(f"Original module vtable/VTT check failed: {error}", file=sys.stderr)
        return 2
    if missing:
        print(f"Original module has unresolved vtable/VTT data: {args.module}", file=sys.stderr)
        for symbol in missing:
            print(f"  {symbol}", file=sys.stderr)
        print("Link the genuine owner or correct its native compiler declarations. "
              "Vtable/VTT data cannot be deferred like an uncalled function. "
              "Host services and lazy method imports are outside this check.", file=sys.stderr)
        return 1
    print(f"Original module: no unresolved game vtables/VTTs ({runtime} C++ runtime table imports).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
