#!/usr/bin/env python3
"""Check that the tweak series leaves the original code when MSCHARGED_TWEAKS is off.

patches/mscharged-decomp-tweaks may only add code that MSCHARGED_TWEAKS
guards: `#if defined(MSCHARGED_TWEAKS)` blocks, whose `#else` branch, if any,
keeps the original lines as they were, and the include of the series' own
header, which is empty without it. Builds without tweaks then compile the
original source unchanged.

The check compares whole files of the prepared tree (PATCHED_DIR, the build's
prepared/mscharged-decomp/patched): each file the series changes, without its
tweak blocks, must have exactly the tokens of that file before the series,
which reversing the series' patches gives back. A file the series adds keeps
nothing but preprocessor lines (its include guard). Without PATCHED_DIR only
the series itself is checked.

usage: test_tweak_patches.py [PATCHED_DIR] [unittest options]
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TWEAKS = ROOT / "patches/mscharged-decomp-tweaks"
INCLUDE = '#include "mscharged_tweaks.h"'
PATCHED = None  # the prepared tree, from the command line


def series():
    return [line.strip() for line in (TWEAKS / "series").read_text().splitlines()
            if line.strip() and not line.startswith("#")]


def changed_files():
    """The files the series changes, adds or removes."""
    files = set()
    for name in series():
        for old, new in re.findall(r"^diff --git a/(\S+) b/(\S+)$", (TWEAKS / name).read_text(), re.M):
            files.update((old, new))
    return sorted(files)


def without_tweaks(lines):
    """The lines a compiler sees without MSCHARGED_TWEAKS: the guarded blocks
    removed but for their #else branch, and the include of the tweak header.

    A block still open at the end of the lines is removed to the end."""
    kept, depth, otherwise, index = [], 0, False, 0
    while index < len(lines):
        text = lines[index].strip()
        directive = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b", text)
        kind = directive.group(1) if directive else None
        if depth == 0 and kind == "if" and "MSCHARGED_TWEAKS" in text:
            if "!defined(MSCHARGED_TWEAKS)" in text.replace(" ", ""):
                raise AssertionError(f"Negated tweak guard: {text}")
            depth, otherwise = 1, False
            while lines[index].rstrip().endswith("\\"):
                index += 1
        elif depth:
            if kind in ("if", "ifdef", "ifndef"):
                depth += 1
            elif kind == "endif":
                depth -= 1
            elif depth == 1 and kind == "elif":
                raise AssertionError(f"#elif in a tweak block: {text}")
            elif depth == 1 and kind == "else":
                # Without tweaks the compiler takes this branch: the original lines.
                otherwise = True
                index += 1
                continue
            if otherwise and depth:
                kept.append(lines[index])
        elif text != INCLUDE:
            kept.append(lines[index])
        index += 1
    return kept


def tokens(lines):
    return re.findall(r"\w+|\S", "\n".join(lines))


def before_series(patched, files, temp):
    """Copies `files` of the prepared tree to `temp` and reverses the series
    there, last patch first: the files as the faithful series left them."""
    for path in files:
        if (patched / path).is_file():
            (temp / path).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(patched / path, temp / path)
    # Plain patch application: no repository around the copies.
    environment = dict(os.environ, GIT_CEILING_DIRECTORIES=str(temp.parent))
    for name in reversed(series()):
        result = subprocess.run(["git", "apply", "-R", "--whitespace=nowarn", str(TWEAKS / name)],
                                cwd=temp, env=environment, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(f"{name} does not reverse on the prepared tree, which may be older than the "
                                 f"series (configure again): {result.stderr.strip()}")


def first_difference(after, before):
    """Both texts around their first different token."""
    a, b = tokens(after), tokens(before)
    at = next((n for n, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
    around = lambda words: " ".join(words[max(0, at - 16):at + 16])
    return f"  before the series: ... {around(b)} ...\n  without tweaks:    ... {around(a)} ..."


class TweakPatchTests(unittest.TestCase):
    def test_series_matches_the_decomp_pin(self):
        base = (ROOT / "patches/mscharged-decomp/base").read_text().strip()
        self.assertEqual((TWEAKS / "base").read_text().strip(), base)
        names = series()
        self.assertTrue(names)
        self.assertEqual(sorted(names), sorted(p.name for p in TWEAKS.glob("*.patch")))
        for name in names:
            self.assertIn("Upstream-base: ", (TWEAKS / name).read_text(), name)
        for path in changed_files():
            self.assertTrue(path.startswith(("src/", "include/", "libs/")), path)

    def test_original_code_without_tweaks(self):
        if PATCHED is None:
            self.skipTest("no prepared tree given (usage: test_tweak_patches.py PATCHED_DIR)")
        self.assertTrue(PATCHED.is_dir(), PATCHED)
        files = changed_files()
        self.assertTrue(files)
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp) / "before"
            temp.mkdir()
            before_series(PATCHED, files, temp)
            for path in files:
                with self.subTest(path=path):
                    after = (PATCHED / path).read_text(errors="replace").splitlines() \
                        if (PATCHED / path).is_file() else None
                    before = (temp / path).read_text(errors="replace").splitlines() \
                        if (temp / path).is_file() else None
                    self.assertIsNotNone(after, "the series must not remove original files")
                    if before is None:
                        # Added by the series: nothing but its include guard without tweaks.
                        left = [line for line in without_tweaks(after)
                                if line.strip() and not line.strip().startswith(("#", "//"))]
                        self.assertEqual(left, [], "a file the series adds must be empty without tweaks")
                    elif tokens(without_tweaks(after)) != tokens(without_tweaks(before)):
                        self.fail("changes the original code:\n" + first_difference(without_tweaks(after),
                                                                                     without_tweaks(before)))

    def test_guard_detection(self):
        new = ["a();", "#if defined(MSCHARGED_TWEAKS) \\", "    && defined(X)", "#if Y", "b();", "#endif",
               "#endif", INCLUDE, "c();"]
        self.assertEqual(without_tweaks(new), ["a();", "c();"])
        with self.assertRaises(AssertionError):
            without_tweaks(["#if !defined(MSCHARGED_TWEAKS)", "x();", "#endif"])
        with self.assertRaises(AssertionError):
            without_tweaks(["#if defined(MSCHARGED_TWEAKS)", "x();", "#elif defined(Y)", "y();", "#endif"])
        # A block still open at the end.
        self.assertEqual(without_tweaks(["a();", "#if defined(MSCHARGED_TWEAKS)", "b();"]), ["a();"])

    def test_else_keeps_the_original(self):
        """An #else branch is what compiles without tweaks, nested conditionals included."""
        lines = ["a();", "#if defined(MSCHARGED_TWEAKS)", "static bool& x = g.x;", "#if Y", "t();", "#else",
                 "u();", "#endif", "#else", "static bool x;", "#if Z", "v();", "#else", "w();", "#endif",
                 "#endif", "b();"]
        self.assertEqual(without_tweaks(lines),
                         ["a();", "static bool x;", "#if Z", "v();", "#else", "w();", "#endif", "b();"])

    def test_blocks_next_to_earlier_ones(self):
        """A block added next to an earlier one passes; a changed original line does not."""
        earlier = ["#if defined(MSCHARGED_TWEAKS)", "x();", "#endif"]
        old = ["a();"] + earlier + ["b();"]
        added = ["#if defined(MSCHARGED_TWEAKS)", "y();", "#endif"]
        self.assertEqual(tokens(without_tweaks(["a();"] + added + earlier + ["b();"])), tokens(without_tweaks(old)))
        self.assertNotEqual(tokens(without_tweaks(["c();"] + earlier + ["b();"])), tokens(without_tweaks(old)))


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        PATCHED = Path(sys.argv.pop(1)).resolve()
    sys.exit(unittest.main())
