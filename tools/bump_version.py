#!/usr/bin/env python3
"""Raise the port version before a main -> stable pull request.

    python3 tools/bump_version.py patch   # bug fixes:        1.0.0 -> 1.0.1
    python3 tools/bump_version.py minor   # new features:     1.0.1 -> 1.1.0
    python3 tools/bump_version.py major   # breaking changes: 1.1.0 -> 2.0.0
    python3 tools/bump_version.py 1.2.3   # an explicit version

The project() line in CMakeLists.txt is the only source of the version; the
README badge follows it. After merging into stable, tag the merge commit
v<version> to publish the release (see docs/BUILDING_GITHUB.md).
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECT = re.compile(r"^(project\(mscharged_port VERSION )(\d+)\.(\d+)\.(\d+)( )", re.MULTILINE)
BADGE = re.compile(r"\[!\[Version [0-9.]+\]\(https://img\.shields\.io/badge/version-[0-9.]+-blue\)\]")


def current(text):
    match = PROJECT.search(text)
    if not match:
        raise SystemExit("CMakeLists.txt has no 'project(mscharged_port VERSION x.y.z ...' line.")
    return tuple(int(part) for part in match.group(2, 3, 4))


def next_version(old, request):
    major, minor, patch = old
    if request == "patch":
        return major, minor, patch + 1
    if request == "minor":
        return major, minor + 1, 0
    if request == "major":
        return major + 1, 0, 0
    if re.fullmatch(r"\d+\.\d+\.\d+", request):
        return tuple(int(part) for part in request.split("."))
    raise SystemExit(f"Expected patch, minor, major or x.y.z, got {request!r}.")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    cmake = ROOT / "CMakeLists.txt"
    readme = ROOT / "README.md"
    cmake_text = cmake.read_text()
    old = current(cmake_text)
    new = next_version(old, sys.argv[1])
    if new <= old:
        raise SystemExit(f"{'.'.join(map(str, new))} is not newer than {'.'.join(map(str, old))}.")
    version = ".".join(map(str, new))
    cmake.write_text(PROJECT.sub(lambda m: f"{m.group(1)}{version}{m.group(5)}", cmake_text, count=1))
    readme_text = readme.read_text()
    if not BADGE.search(readme_text):
        raise SystemExit("README.md has no version badge to update.")
    readme.write_text(BADGE.sub(
        f"[![Version {version}](https://img.shields.io/badge/version-{version}-blue)]", readme_text, count=1))
    print(f"{'.'.join(map(str, old))} -> {version}")
    print(f"Commit, open the main -> stable pull request, merge, then tag the merge: v{version}")


if __name__ == "__main__":
    main()
