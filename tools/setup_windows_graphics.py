#!/usr/bin/env python3
"""Acquire the pinned x64 Windows shader compiler DLLs and their notices.

Downloads only on an explicit setup run. --check is offline and is used by
configuration, builds, and packaging. No Windows system/driver DLLs are copied.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
import urllib.request
import zipfile

PIN = Path(__file__).with_name("windows_graphics.json")
DEFAULT_OUTPUT = Path(__file__).resolve().parents[1] / "build/windows-graphics"


def digest(path):
    with path.open("rb") as stream:
        hasher = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def file_records(pin):
    return [record for archive in pin["archives"] for record in archive["files"]] + pin["notices"]


def verify(root, pin=None):
    pin = pin or json.loads(PIN.read_text())
    for record in file_records(pin):
        path = root / record["path"]
        if path.is_symlink() or not path.is_file() or digest(path) != record["sha256"]:
            raise ValueError(f"Missing or modified Windows graphics dependency: {path}. "
                             "Run tools/setup_windows_graphics.py.")
    manifest = root / "windows-graphics.json"
    if not manifest.is_file() or json.loads(manifest.read_text()) != pin:
        raise ValueError(f"Stale Windows graphics provenance: {manifest}")
    return pin


def download(url, expected, destination):
    if destination.is_file() and digest(destination) == expected:
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Partial or rejected downloads never replace a verified cached archive.
    partial = None
    try:
        with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as output:
            partial = Path(output.name)
            with urllib.request.urlopen(url, timeout=120) as response:
                shutil.copyfileobj(response, output)
        if digest(partial) != expected:
            raise ValueError(f"SHA-256 mismatch for {url}")
        os.replace(partial, destination)
    finally:
        if partial is not None:
            partial.unlink(missing_ok=True)


def setup(root, cache):
    pin = json.loads(PIN.read_text())
    try:
        verify(root, pin)
        return
    except ValueError:
        pass
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="windows-graphics-", dir=root.parent) as temp:
        stage = Path(temp)
        for archive in pin["archives"]:
            path = cache / (archive["sha256"] + ".zip")
            download(archive["url"], archive["sha256"], path)
            with zipfile.ZipFile(path) as source:
                # Extract only the pinned files, never arbitrary archive paths.
                for record in archive["files"]:
                    target = stage / record["path"]
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(source.read(record["member"]))
        for record in pin["notices"]:
            path = cache / record["sha256"]
            download(record["url"], record["sha256"], path)
            target = stage / record["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
        (stage / "windows-graphics.json").write_text(json.dumps(pin, indent=2) + "\n")
        verify(stage, pin)
        for record in file_records(pin):
            target = root / record["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            os.replace(stage / record["path"], target)
        os.replace(stage / "windows-graphics.json", root / "windows-graphics.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--cache-dir", type=Path, default=DEFAULT_OUTPUT.parent / "downloads/windows-graphics")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        if args.check:
            verify(args.output_dir)
        else:
            setup(args.output_dir, args.cache_dir)
        print(f"Verified Windows graphics DLLs: {args.output_dir}")
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Windows graphics setup failed: {error}\n")


if __name__ == "__main__":
    main()
