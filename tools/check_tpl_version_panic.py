#!/usr/bin/env python3
"""Require the actual original TPL.c invalid-version OSPanic to abort."""
import argparse
import subprocess
import signal
import sys


def no_core():
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", required=True)
    args = parser.parse_args()
    kwargs = {}
    if sys.platform != "win32":
        kwargs["preexec_fn"] = no_core
    run = subprocess.run([args.program, "version-panic"], capture_output=True,
                         text=True, timeout=5, **kwargs)
    sys.stdout.write(run.stdout)
    sys.stderr.write(run.stderr)
    text = run.stdout + run.stderr
    aborted = run.returncode == -signal.SIGABRT or (sys.platform == "win32" and run.returncode == 3)
    if not aborted or "invalid version number for texture palette" not in text or '"TPL.c" on line 25' not in text:
        print(f"Original TPL panic was not observed (exit {run.returncode})", file=sys.stderr)
        return 1
    print("Whole original TPL.c invalid-version OSPanic observed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
