#!/usr/bin/env python3
"""The Discord Social SDK for the launcher's Discord friends (cmake/DiscordSocialSdk.cmake).

It is Discord's closed library: its terms allow shipping it inside Recharged,
not passing it on by itself, so it stays out of the repository and comes from
a private place instead.

bundle  makes the files builds need (the C header, the notices and the release
        libraries of Linux, macOS and Windows x64/arm64) from Discord's download,
        a zip of about 800 MB, into a small zip for that private place, and
        prints its SHA-256
fetch   (CI) downloads that zip from DISCORD_SOCIAL_SDK_URL, with the bearer
        token DISCORD_SOCIAL_SDK_TOKEN when set (a GitHub release asset's API
        URL with a token that can read it works), checks it against
        DISCORD_SOCIAL_SDK_SHA256 when set, unpacks it into DESTINATION and
        prints DISCORD_SDK=<folder> for $GITHUB_ENV; without a URL it prints
        nothing, and the build goes without Discord friends
"""

import argparse
import hashlib
import os
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

ROOT = "discord_social_sdk"
FILES = (
    "License-Notices.txt",
    "include/cdiscord.h",
    "lib/release/libdiscord_partner_sdk.so",
    "lib/release/libdiscord_partner_sdk.dylib",
    "bin/release/discord_partner_sdk.dll",
    "bin/release/arm64/discord_partner_sdk.dll",
)
# Far above the bundle's size (about 60 MB unpacked).
MAX_BYTES = 256 << 20


def bundle(source, output):
    with zipfile.ZipFile(source) as download, zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as small:
        names = set(download.namelist())
        for name in FILES:
            member = f"{ROOT}/{name}"
            if member not in names:
                raise SystemExit(f"{source} has no {member}: not the Discord Social SDK download?")
            small.writestr(member, download.read(member))
    return hashlib.sha256(Path(output).read_bytes()).hexdigest()


def fetch(destination):
    url = os.environ.get("DISCORD_SOCIAL_SDK_URL", "").strip()
    if not url:
        print("No DISCORD_SOCIAL_SDK_URL: building without the Discord Social SDK", file=sys.stderr)
        return None
    request = urllib.request.Request(url, headers={"Accept": "application/octet-stream",
                                                   "User-Agent": "recharged-ci"})
    token = os.environ.get("DISCORD_SOCIAL_SDK_TOKEN", "").strip()
    if token:
        # Not sent on to where a release asset redirects.
        request.add_unredirected_header("Authorization", f"Bearer {token}")
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryFile() as archive:
        with urllib.request.urlopen(request, timeout=120) as response:
            size = 0
            while chunk := response.read(1 << 20):
                size += len(chunk)
                if size > MAX_BYTES:
                    raise SystemExit("The Discord Social SDK download is too large")
                archive.write(chunk)
        archive.seek(0)
        digest = hashlib.file_digest(archive, "sha256").hexdigest()
        expected = os.environ.get("DISCORD_SOCIAL_SDK_SHA256", "").strip().lower()
        if expected and digest != expected:
            raise SystemExit(f"The Discord Social SDK download is {digest}, not {expected}")
        archive.seek(0)
        with zipfile.ZipFile(archive) as bundle_zip:
            for name in FILES:
                member = f"{ROOT}/{name}"
                path = PurePosixPath(member)
                if path.is_absolute() or ".." in path.parts:
                    raise SystemExit(f"Unsafe path {member}")
                target = destination / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(bundle_zip.read(member))
    print(f"Discord Social SDK {digest[:12]} in {destination / ROOT}", file=sys.stderr)
    return destination / ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    make = commands.add_parser("bundle", help="the small zip for the private place")
    make.add_argument("download", type=Path, help="DiscordSocialSdk-<version>.zip from Discord")
    make.add_argument("output", type=Path)
    get = commands.add_parser("fetch", help="(CI) the small zip, unpacked")
    get.add_argument("destination", type=Path)
    args = parser.parse_args()
    if args.command == "bundle":
        print(bundle(args.download, args.output))
    else:
        folder = fetch(args.destination)
        if folder:
            print(f"DISCORD_SDK={folder}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
