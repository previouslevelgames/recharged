#!/usr/bin/env python3
"""Check that every tweak hook sits in code that all game versions compile.

The game modules of the USA and European releases build the same prepared
sources with different VERSION_* macros. A MSCHARGED_TWEAKS block inside a
VERSION_ conditional would exist in one module only, so its tweak or fix would
work with one disc and silently not with the other.

usage: test_tweak_versions.py PREPARED_SOURCE_DIR
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TWEAKS = ROOT / "patches/mscharged-decomp-tweaks"
DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")


def hooked_files():
    names = [line.strip() for line in (TWEAKS / "series").read_text().splitlines()
             if line.strip() and not line.startswith("#")]
    files = set()
    for name in names:
        files.update(re.findall(r"^\+\+\+ b/(\S+)", (TWEAKS / name).read_text(), re.M))
    return sorted(files)


def misplaced_hooks(path):
    """Lines of MSCHARGED_TWEAKS blocks that a VERSION_ conditional encloses."""
    stack, found = [], []
    for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        match = DIRECTIVE.match(line)
        if not match:
            continue
        kind, rest = match.groups()
        if kind in ("if", "ifdef", "ifndef"):
            if "MSCHARGED_TWEAKS" in rest and any(stack):
                found.append(number)
            stack.append("VERSION_" in rest)
        elif kind == "elif":
            stack[-1] = stack[-1] or "VERSION_" in rest
        elif kind == "endif":
            stack.pop()
    return found


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    prepared = Path(sys.argv[1])
    files = hooked_files()
    problems = []
    for name in files:
        problems += [f"{name}:{line}" for line in misplaced_hooks(prepared / name)]
    if problems:
        print("Tweak hooks inside version conditionals:\n  " + "\n  ".join(problems))
        return 1
    print(f"Tweak hooks of {len(files)} files are outside version conditionals.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
