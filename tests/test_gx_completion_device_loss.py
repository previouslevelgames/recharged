#!/usr/bin/env python3
"""Actual device loss is a failure, even though Dawn work-done can say Success."""
import subprocess
import sys

result = subprocess.run([sys.argv[1], "--device-loss"], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True, timeout=35)
output = result.stdout
print(output)
if result.returncode == 0:
    raise SystemExit("Lost device incorrectly returned success")
if "Forcing device loss after " not in output:
    raise SystemExit("Probe failed before the device-loss gate")
if "Device lost: GX completion failure qualification" not in output:
    raise SystemExit("The actual native device-loss callback did not report the failure")
if "GX physical completion probe passed" in output or "lost device incorrectly reported completion" in output:
    raise SystemExit("Failed device was reported as completed")
if any(word in output for word in ("VUID-", "Validation Error", "Error:", "GX completion probe failed")):
    raise SystemExit("Unexpected host/validation failure before the loss boundary")
print("Actual device-loss failure verified")
