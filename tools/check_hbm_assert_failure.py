#!/usr/bin/env python3
"""Observe genuine original HBM Panic; it must halt, never return success."""
import signal
import subprocess
import sys

binary, mode = sys.argv[1:]
if mode not in ('panic', 'alignment'):
    raise SystemExit('Expected a qualified original assertion failure mode')
result = subprocess.run([binary, mode], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True, timeout=8)
print(result.stdout, end='')
expected = 'retail-hbm-assertion:540 Panic:' if mode == 'panic' else '100000021'
if result.returncode != -signal.SIGABRT or 'Panic:' not in result.stdout or expected not in result.stdout:
    raise SystemExit('Original HBM assertion did not reach its qualified native halt')
