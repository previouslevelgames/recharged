#!/usr/bin/env python3
"""Sign the macOS package folder or its disk image for release.

FOLDER (a package folder: Recharged.app and the game modules beside it): the
modules and the app are signed with the Developer ID and the hardened runtime,
the folder is notarized by Apple and the ticket stapled to the app.
IMAGE.dmg: signed with the Developer ID, notarized and stapled.

Gatekeeper then opens them without the Privacy & Security override. Without
the certificate (local and fork builds) a folder is signed ad hoc as the build
does, and an image stays unsigned; without the notary key only signed.

MACOS_CERTIFICATE           base64 of the Developer ID Application .p12
MACOS_CERTIFICATE_PASSWORD  its export password
MACOS_NOTARY_KEY            an App Store Connect API key (.p8 text)
MACOS_NOTARY_KEY_ID, MACOS_NOTARY_ISSUER  that key's ID and issuer ID
"""

import argparse
import base64
import json
import os
import secrets
import subprocess
import tempfile
from pathlib import Path

from macos_package import MACHO_MAGIC, retried
from package_build import MACOS_APP


def run(*args, **options):
    subprocess.run([str(arg) for arg in args], check=True, **options)


def output(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True)



class Keychain:
    """A temporary keychain holding only the Developer ID certificate."""

    def __init__(self, folder, certificate, password):
        self.path = Path(folder) / "signing.keychain-db"
        self.password = secrets.token_hex(16)
        self.previous = [line.strip().strip('"') for line in
                         output("security", "list-keychains", "-d", "user").splitlines() if line.strip()]
        p12 = Path(folder) / "certificate.p12"
        p12.write_bytes(base64.b64decode(certificate))
        run("security", "create-keychain", "-p", self.password, self.path)
        try:
            run("security", "set-keychain-settings", "-lut", "21600", self.path)
            run("security", "unlock-keychain", "-p", self.password, self.path)
            run("security", "import", p12, "-k", self.path, "-P", password, "-T", "/usr/bin/codesign")
            run("security", "set-key-partition-list", "-S", "apple-tool:,apple:,codesign:", "-s",
                "-k", self.password, self.path, stdout=subprocess.DEVNULL)
            run("security", "list-keychains", "-d", "user", "-s", self.path, *self.previous)
        except BaseException:
            self.close()
            raise
        finally:
            p12.unlink()
        identities = output("security", "find-identity", "-v", "-p", "codesigning", self.path)
        found = [line.split()[1] for line in identities.splitlines() if "Developer ID Application" in line]
        if not found:
            self.close()
            raise SystemExit("The certificate is not a Developer ID Application identity")
        self.identity = found[0]

    def close(self):
        subprocess.run(["security", "list-keychains", "-d", "user", "-s", *self.previous], check=False)
        subprocess.run(["security", "delete-keychain", str(self.path)], check=False)


def notarize(item, folder):
    """Submits `item` (a package folder, zipped first, or a disk image) and staples
    the ticket to the image, or to the folder's app."""
    key = os.environ.get("MACOS_NOTARY_KEY")
    if not key:
        print("Signed with the Developer ID; not notarized (needs MACOS_NOTARY_KEY and its ID and issuer).")
        return
    key_file = Path(folder) / "notary.p8"
    key_file.write_text(key)
    submission, staple = item, item
    if item.is_dir():
        submission = Path(folder) / (item.name + ".zip")
        run("ditto", "-c", "-k", "--keepParent", item, submission)
        staple = item / MACOS_APP
    credentials = ["--key", key_file, "--key-id", os.environ["MACOS_NOTARY_KEY_ID"],
                   "--issuer", os.environ["MACOS_NOTARY_ISSUER"]]
    result = json.loads(output("xcrun", "notarytool", "submit", submission, *credentials,
                               "--wait", "--timeout", "60m", "--output-format", "json"))
    if result.get("status") != "Accepted":
        subprocess.run(["xcrun", "notarytool", "log", result.get("id", ""), *map(str, credentials)])
        raise SystemExit(f"Notarization {result.get('status')}: {result.get('message', '')}")
    retried("xcrun", "stapler", "staple", staple)
    retried("xcrun", "stapler", "validate", staple)
    if item.is_dir():
        retried("spctl", "--assess", "--type", "execute", "--verbose=2", staple)
    else:
        retried("spctl", "--assess", "--type", "open", "--context", "context:primary-signature", "--verbose=2", staple)


def modules(folder):
    """The game modules: the programs beside the app."""
    found = []
    for path in sorted(folder.iterdir()):
        if path.is_file() and not path.is_symlink():
            with path.open("rb") as stream:
                if stream.read(4) in MACHO_MAGIC:
                    found.append(path)
    return found


def sign(item):
    if item.is_dir():
        if not (item / MACOS_APP).is_dir():
            raise SystemExit(f"Not a package folder (no {MACOS_APP}): {item}")
        # Inside out: the modules, then the app (which signs its program).
        items = [*modules(item), item / MACOS_APP]
    elif item.suffix == ".dmg":
        items = [item]
    else:
        raise SystemExit(f"Expected a package folder or a .dmg: {item}")
    certificate = os.environ.get("MACOS_CERTIFICATE")
    if not certificate:
        if not item.is_dir():
            print(f"No Developer ID certificate: {item.name} stays unsigned.")
            return
        print("No Developer ID certificate: ad-hoc signatures; Gatekeeper asks the player to allow the app.")
        for path in items:
            run("codesign", "--force", "--sign", "-", path)
            run("codesign", "--verify", "--strict", path)
        return
    with tempfile.TemporaryDirectory() as folder:
        keychain = Keychain(folder, certificate, os.environ.get("MACOS_CERTIFICATE_PASSWORD", ""))
        try:
            # The hardened runtime for programs; an image has no runtime.
            options = ["--options", "runtime"] if item.is_dir() else []
            for path in items:
                run("codesign", "--force", "--timestamp", *options, "--keychain", keychain.path,
                    "--sign", keychain.identity, path)
                run("codesign", "--verify", "--strict", "--verbose=2", path)
        finally:
            keychain.close()
        notarize(item, folder)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("item", type=Path, help="a package folder (Recharged.app beside its modules) or a .dmg")
    sign(parser.parse_args().item.resolve())
