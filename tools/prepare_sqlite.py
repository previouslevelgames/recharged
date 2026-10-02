#!/usr/bin/env python3
"""Generate and verify SQLite's amalgamation from its prepared, pinned Git source."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from prepare_sources import PreparationError, content_inventory, encoded, preparation_lock, prepare, sha


def tool_info(executable, *args, input=None):
    executable = str(Path(executable).resolve())
    result = subprocess.run([executable, *args], input=input, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
    return {"path": executable, "version": result.stdout.strip()}


def generate(root, build, cc, make, tclsh, *, check=False, discard_generated=False):
    build = build.resolve()
    source = prepare(root, build, "sqlite", check=check)
    source_state = json.loads((source.parent / "manifest.json").read_text())
    inputs = {"format": 1, "source_key": source_state["key"],
              "generator_sha256": sha(Path(__file__).read_bytes()),
              "cc": tool_info(cc, "--version"), "make": tool_info(make, "--version"),
              "tclsh": tool_info(tclsh, input="puts [info patchlevel]\n")}
    key = sha(encoded(inputs))
    parent = build / "generated"
    parent.mkdir(parents=True, exist_ok=True)
    target = parent / "sqlite-source"
    with preparation_lock(parent / ".sqlite.lock"):
        state = None
        if (target / "manifest.json").is_file():
            state = json.loads((target / "manifest.json").read_text())
        inventory = content_inventory(target) if target.is_dir() else {}
        inventory.pop("manifest.json", None)
        clean = state is not None and inventory == state.get("content")
        current = state is not None and state.get("key") == key
        if check:
            if not current or not clean:
                raise PreparationError("SQLite generated inputs are stale or modified; rerun configuration and preserve edits before regenerating")
            return target
        if target.exists() and not clean and not discard_generated:
            raise PreparationError("SQLite generated inputs were edited; preserve them before using --discard-generated")
        if current and clean:
            return target
        with tempfile.TemporaryDirectory(prefix=".sqlite-generate-", dir=parent) as temporary:
            work = Path(temporary)
            log = build / "sqlite-generation.log"
            command = [str(make), "-f", str(source / "Makefile.linux-generic"),
                       f"TOP={source}", f"B.cc={cc}", f"B.tclsh={tclsh}",
                       f"TCLSH_CMD={tclsh}", "CFLAGS=", "sqlite3.c", "sqlite3.h"]
            environment = {k: v for k, v in os.environ.items()
                           if k not in ("MAKEFLAGS", "MFLAGS", "GNUMAKEFLAGS")}
            with log.open("w") as output:
                result = subprocess.run(command, cwd=work, env=environment,
                                        stdout=output, stderr=subprocess.STDOUT)
            if result.returncode:
                raise PreparationError(f"SQLite source generation failed; see {log}")
            ready = work / "ready"
            ready.mkdir()
            for name in ("sqlite3.c", "sqlite3.h"):
                shutil.copyfile(work / name, ready / name)
            state = {"key": key, "inputs": inputs, "content": content_inventory(ready)}
            (ready / "manifest.json").write_text(json.dumps(state, indent=2, sort_keys=True) + "\n")
            previous = work / "previous"
            if target.exists():
                target.rename(previous)
            try:
                ready.rename(target)
            except OSError:
                if previous.exists():
                    previous.rename(target)
                raise
        return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--make", required=True)
    parser.add_argument("--tclsh", required=True)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--check", action="store_true")
    modes.add_argument("--discard-generated", action="store_true")
    args = parser.parse_args()
    try:
        print(generate(Path(__file__).resolve().parents[1], args.build_dir,
                       args.cc, args.make, args.tclsh, check=args.check,
                       discard_generated=args.discard_generated))
    except (PreparationError, OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"SQLite preparation failed: {error}\n")


if __name__ == "__main__":
    main()
