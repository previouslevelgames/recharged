#!/usr/bin/env python3
"""Checks Recharged's texts (assets/lang, src/bootstrap/texts.h).

- Every ID the code names (a string literal as the first argument of T or
  Count) has its English text in assets/lang/en; Count's IDs have plural
  forms there (ID_one, ID_other).
- Every translation's file parses, holds only texts (strings in nested
  objects) and only IDs English has, and each text has the placeholders
  ({{name}}) of its English text.
English texts no code names are listed as a note (IDs from tables, such as a
setting's choices, are built at run time and not seen here).

Usage: check_texts.py SOURCE_DIR
"""

import json
import re
import sys
from pathlib import Path

PLURAL = ("zero", "one", "two", "few", "many", "other")
PLACEHOLDER = re.compile(r"\{\{\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}\}")
# T("id"...) or Count("id"...), also texts::T, not part of a longer name.
CALL = re.compile(r'(?<![A-Za-z0-9_])(T|Count)\(\s*"([a-z][a-z0-9_]*(?:\.[a-z0-9_]+)+)"')


def flatten(value, prefix, out, problems, where):
    if isinstance(value, str):
        out[prefix] = value
    elif isinstance(value, dict):
        for name, item in value.items():
            if "." in name:
                problems.append(f"{where}: key {name!r} has a dot (nest it instead)")
            flatten(item, f"{prefix}.{name}", out, problems, where)
    else:
        problems.append(f"{where}: {prefix} is not a text")


def read_language(folder, problems):
    texts = {}
    for path in sorted(folder.glob("*.json")):
        where = path.as_posix()
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            problems.append(f"{where}: {error}")
            continue
        if not isinstance(document, dict):
            problems.append(f"{where}: not an object")
            continue
        flatten(document, path.stem, texts, problems, where)
    return texts


def base(id_):
    head, _, tail = id_.rpartition("_")
    return head if head and tail in PLURAL else id_


def placeholders(text):
    return set(PLACEHOLDER.findall(text))


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    source = Path(sys.argv[1])
    lang = source / "assets" / "lang"
    problems = []
    english = read_language(lang / "en", problems)
    if not english:
        problems.append("assets/lang/en has no texts")
    english_bases = {base(id_) for id_ in english}

    used = set()
    for path in sorted((source / "src").rglob("*")):
        if path.suffix not in (".cpp", ".h", ".inc"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in CALL.finditer(text):
            kind, id_ = match.group(1), match.group(2)
            used.add(id_)
            where = f"{path.relative_to(source).as_posix()}:{text.count(chr(10), 0, match.start()) + 1}"
            if kind == "Count":
                if f"{id_}_other" not in english:
                    problems.append(f"{where}: {id_} has no plural forms in English ({id_}_one, {id_}_other)")
            elif id_ not in english:
                problems.append(f"{where}: {id_} has no English text")

    for folder in sorted(p for p in lang.iterdir() if p.is_dir() and p.name != "en"):
        texts = read_language(folder, problems)
        for id_, text in texts.items():
            where = f"assets/lang/{folder.name}: {id_}"
            if id_ not in english and base(id_) not in english_bases:
                problems.append(f"{where}: no such text in English")
                continue
            if not text:
                continue
            reference = english.get(id_) or english.get(f"{base(id_)}_other", "")
            missing = placeholders(reference) - placeholders(text)
            extra = placeholders(text) - placeholders(reference)
            if base(id_) != id_:
                missing.discard("count")
            if missing:
                problems.append(f"{where}: lacks {', '.join('{{' + p + '}}' for p in sorted(missing))}")
            if extra:
                problems.append(f"{where}: has unknown {', '.join('{{' + p + '}}' for p in sorted(extra))}")

    unused = sorted(b for b in english_bases if b not in used)
    if unused:
        print(f"note: {len(unused)} English texts no code names directly (tables build these IDs)")
    for problem in problems:
        print(problem)
    print(f"{len(english)} English texts, {len(used)} IDs in the code, {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
