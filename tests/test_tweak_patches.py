#!/usr/bin/env python3
"""Check that the tweak series leaves the original code when MSCHARGED_TWEAKS is off.

patches/mscharged-decomp-tweaks may only add code guarded by MSCHARGED_TWEAKS
(plus the include of its own header, which is empty without it). For every
hunk, the new text with those blocks removed must have exactly the tokens of
the old text, so builds without tweaks compile the original source unchanged.
"""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TWEAKS = ROOT / "patches/mscharged-decomp-tweaks"
HEADER = "include/mscharged_tweaks.h"
INCLUDE = '#include "mscharged_tweaks.h"'


def series():
    return [line.strip() for line in (TWEAKS / "series").read_text().splitlines()
            if line.strip() and not line.startswith("#")]


def hunks(patch):
    """(path, old lines, new lines) of each hunk."""
    path, old, new, result = None, None, None, []
    for line in patch.splitlines():
        if line.startswith("diff --git "):
            path = None
        elif line.startswith("+++ "):
            path = line[4:].removeprefix("b/")
        elif line.startswith("@@"):
            old, new = [], []
            result.append((path, old, new))
        elif old is not None and line[:1] in (" ", "-", "+") and not line.startswith(("--- ", "+++ ")):
            if line[0] in " -":
                old.append(line[1:])
            if line[0] in " +":
                new.append(line[1:])
    return result


def without_tweaks(lines):
    """The lines a compiler sees without MSCHARGED_TWEAKS (guarded blocks and the header include removed)."""
    kept, depth, index = [], 0, 0
    while index < len(lines):
        text = lines[index].strip()
        directive = re.match(r"#\s*(if|ifdef|ifndef|endif)\b", text)
        if depth == 0 and directive and directive.group(1) == "if" and "MSCHARGED_TWEAKS" in text:
            if "!defined(MSCHARGED_TWEAKS)" in text.replace(" ", ""):
                raise AssertionError(f"Negated tweak guard: {text}")
            depth = 1
            while lines[index].rstrip().endswith("\\"):
                index += 1
        elif depth:
            if directive and directive.group(1) in ("if", "ifdef", "ifndef"):
                depth += 1
            elif directive and directive.group(1) == "endif":
                depth -= 1
        elif text != INCLUDE:
            kept.append(lines[index])
        index += 1
    if depth:
        raise AssertionError("Unclosed MSCHARGED_TWEAKS block")
    return kept


def tokens(lines):
    return re.findall(r"\w+|\S", "\n".join(lines))


class TweakPatchTests(unittest.TestCase):
    def test_series_matches_the_decomp_pin(self):
        base = (ROOT / "patches/mscharged-decomp/base").read_text().strip()
        self.assertEqual((TWEAKS / "base").read_text().strip(), base)
        names = series()
        self.assertTrue(names)
        self.assertEqual(sorted(names), sorted(p.name for p in TWEAKS.glob("*.patch")))

    def test_original_code_without_tweaks(self):
        checked = 0
        for name in series():
            text = (TWEAKS / name).read_text()
            self.assertIn("Upstream-base: ", text, name)
            for path, old, new in hunks(text):
                with self.subTest(patch=name, path=path):
                    if path == HEADER:
                        body = [line for line in new if line.strip()]
                        self.assertIn("#if defined(MSCHARGED_TWEAKS)", body)
                        checked += 1
                        continue
                    self.assertTrue(path.startswith(("src/", "include/", "libs/")), path)
                    self.assertEqual(tokens(without_tweaks(new)), tokens(old))
                    checked += 1
        self.assertGreater(checked, 0)

    def test_guard_detection(self):
        new = ["a();", "#if defined(MSCHARGED_TWEAKS) \\", "    && defined(X)", "#if Y", "b();", "#endif",
               "#endif", INCLUDE, "c();"]
        self.assertEqual(without_tweaks(new), ["a();", "c();"])
        with self.assertRaises(AssertionError):
            without_tweaks(["#if !defined(MSCHARGED_TWEAKS)", "x();", "#endif"])


if __name__ == "__main__":
    sys.exit(unittest.main())
