#!/usr/bin/env python3
"""Generated CPU source callback gate; contains no retail data."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

from generate_original_frontend_render_fixture import generate


with tempfile.TemporaryDirectory(prefix='original frontend render ') as temporary:
    output = Path(temporary) / 'fixture with spaces'
    generate(output)
    environment = os.environ.copy()
    environment.update(SDL_VIDEODRIVER='dummy', SDL_RENDER_DRIVER='software', SDL_AUDIODRIVER='dummy')
    result = subprocess.run([str(Path(sys.argv[1]).resolve()), str(output / 'manifest.txt')],
                            capture_output=True, text=True, env=environment, timeout=90)
    if result.returncode:
        print(result.stdout, end='')
        print(result.stderr, end='', file=sys.stderr)
        raise SystemExit(result.returncode)
    summaries = [line for line in result.stdout.splitlines()
                 if line.startswith('actual original frontend render checks=')]
    if len(summaries) != 1:
        raise SystemExit('Original source render qualifier did not report its completion')
    print(summaries[0])
