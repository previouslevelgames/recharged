#!/usr/bin/env python3
"""Stage the measured LLVM-MinGW shared C++ imports for the Windows ABI check."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stage(readobj, runtime_dir, output_dir, images, manifest):
    output_dir.mkdir(parents=True, exist_ok=True)
    local = {p.name.lower() for p in images}
    available = {p.name.lower(): p for p in runtime_dir.glob("*.dll")}
    queue, copied, graph = list(images), {}, []
    while queue:
        path = queue.pop(0)
        before = digest(path)
        text = subprocess.check_output([str(readobj), "--coff-imports", str(path)], text=True)
        imports = re.findall(r"^  Name: ([^\r\n]+)", text, re.M)
        if digest(path) != before:
            raise RuntimeError(f"PE image changed while inspecting imports: {path}")
        graph.append({"image": str(path), "sha256": before, "imports": imports})
        for name in imports:
            key = name.lower()
            if key in local or key in copied:
                continue
            if key == "kernel32.dll" or key.startswith(("api-ms-win-", "ext-ms-win-")):
                continue  # Genuine OS/UCRT API-set contract, not shipped shims.
            if key not in ("libc++.dll", "libunwind.dll") or key not in available:
                raise RuntimeError(f"Unqualified shared-runtime dependency: {name}")
            source = available[key]
            source_hash = digest(source)
            target = output_dir / source.name
            if not target.exists() or digest(target) != source_hash:
                temporary = output_dir / (source.name + ".stage.tmp")
                shutil.copyfile(source, temporary)
                if digest(source) != source_hash or digest(temporary) != source_hash:
                    temporary.unlink()
                    raise RuntimeError(f"Runtime DLL changed while staging: {source}")
                temporary.replace(target)
            copied[key] = {"source": str(source), "staged": str(target), "sha256": source_hash}
            queue.append(target)
    if set(copied) != {"libc++.dll", "libunwind.dll"}:
        raise RuntimeError("ABI check must actually import the shared libc++/unwind pair")
    if any("libc++.dll" not in [n.lower() for n in row["imports"]] for row in graph[:len(images)]):
        raise RuntimeError("Every fixture image must use the actual shared libc++ provider")
    record = {"images": graph, "staged_runtimes": copied,
              "system_contract": "Kernel32 and OS-provided UCRT API sets; no packaged API-set substitutes"}
    manifest.write_text(json.dumps(record, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--readobj", required=True, type=Path)
    parser.add_argument("--runtime-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--image", action="append", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    args = parser.parse_args()
    stage(args.readobj, args.runtime_dir, args.output_dir, args.image, args.manifest)


if __name__ == "__main__":
    main()
