#!/usr/bin/env python3
"""Run the original SRAM/RTC leaf with explicitly disposable backing data."""
import subprocess
import sys
import tempfile

if len(sys.argv) != 2:
    raise SystemExit("usage: run_native_rtc.py EXECUTABLE")
with tempfile.TemporaryDirectory(prefix="mscharged-native-rtc-") as directory:
    result = subprocess.run([sys.argv[1], directory], timeout=15, check=False)
    raise SystemExit(result.returncode)
