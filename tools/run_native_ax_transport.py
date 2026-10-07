#!/usr/bin/env python3
"""Run an AX hardware qualifier with the actual source-image identity."""

import hashlib
from pathlib import Path
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: run_native_ax_transport.py EXECUTABLE SOURCE_IMAGE", file=sys.stderr)
        return 2
    executable, image = (Path(value).resolve() for value in sys.argv[1:])
    if not executable.is_file() or not image.is_file():
        print("AX qualifier executable or source image is absent", file=sys.stderr)
        return 2
    digest = hashlib.sha256()
    with image.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    try:
        result = subprocess.run([str(executable), str(image), digest.hexdigest()], timeout=30)
    except subprocess.TimeoutExpired:
        print("AX hardware qualifier timed out", file=sys.stderr)
        return 1
    return result.returncode if result.returncode >= 0 else 1


if __name__ == "__main__":
    sys.exit(main())
