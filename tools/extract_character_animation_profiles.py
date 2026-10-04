#!/usr/bin/env python3
"""Extract the checked animation fields from the pinned original character table."""
import argparse
import json
from pathlib import Path
import re

FIELDS = ("cc", "szModelFilename", "szShockModelFilename", "szLowPolyModelFilename",
          "szShadowModelFilename", "szTextureFilename", "szAlternateTextureFilename",
          "szTriggerFilename", "pTriggerCallback", "szHierarchyFilename", "szHierarchy",
          "pAnimProperties", "nNumAnimProperties", "szAnimFilename", "szFEAnimFilename",
          "szEffectsName", "szPhysicsFilename", "szTweaksFilename", "szTweaksCategory",
          "szSuperTweaksFilename", "szSuperTweaksCategory", "szAnimRetargetFilename",
          "bTexturesLoaded", "pad_0x59")
TOKEN = r'"(?:\\.|[^"\\])*"'


def uncomment(text):
    return re.sub(TOKEN + r"|//[^\n]*|/\*.*?\*/", lambda m: m[0] if m[0].startswith('"') else " ", text, flags=re.S)


def extract(source, header):
    source, header = uncomment(source), uncomment(header)
    declaration = re.search(r"struct\s+tCharacterTemplateInfo\s*\{([^{}]*)\}\s*;", header)
    if not declaration:
        raise ValueError("Original character template declaration is missing")
    fields = []
    for line in declaration[1].split(";")[:-1]:
        match = re.search(r"\(\s*\*\s*(\w+)\s*\)", line) or re.search(r"(\w+)\s*(?:\[[^]]+\])?\s*$", line)
        if not match:
            raise ValueError("Unknown original character field declaration")
        fields.append(match[1])
    if tuple(fields) != FIELDS:
        raise ValueError("Original character field order changed; review profile extraction")
    table = re.search(r"static\s+tCharacterTemplateInfo\s+g_aCharacterTemplateInfo\s*\[\s*20\s*\]\s*=\s*\{(.*?)\}\s*;", source, re.S)
    if not table:
        raise ValueError("Original 20-character table is missing")
    rows, cursor = [], 0
    for match in re.finditer(r"\{([^{}]*)\}", table[1]):
        separator = table[1][cursor:match.start()].strip()
        if separator != ("," if rows else ""):
            raise ValueError("Unrecognized expression in original character table")
        cursor = match.end()
        # This source profile allows literals and identifiers, not evaluated C++.
        # Commas inside a string remain part of that literal.
        parts = re.split(r',(?=(?:[^"\\]*(?:\\.|"(?:\\.|[^"\\])*"))*[^"\\]*$)', match[1])
        parts = [p.strip() for p in parts]
        if parts and not parts[-1]:
            parts.pop()
        if len(parts) != 23:
            raise ValueError("Original character initializer must contain 23 fields")
        index = len(rows)
        if not re.fullmatch(r"\(eCharacterClass\)\s*" + str(index), parts[0]):
            raise ValueError("Original character indexes must retain source order 0..19")
        for i, value in enumerate(parts[1:], 1):
            if i in (8, 11):
                valid = re.fullmatch(r"[A-Za-z_]\w*", value)
            elif i in (12, 22):
                valid = re.fullmatch(r"[0-9]+", value)
            else:
                valid = value == "NULL" or re.fullmatch(TOKEN, value)
            if not valid:
                raise ValueError(f"Unrecognized original character field {FIELDS[i]}")
        values = []
        for i in (10, 9, 14, 21):
            value = json.loads(parts[i])  # Required, literal paths; never NULL.
            if not isinstance(value, str) or not value or len(value) > 255 or any(ord(c) < 32 or ord(c) >= 127 for c in value):
                raise ValueError("Character animation profile needs bounded printable source literals")
            values.append(value)
        name, hierarchy, animation, retarget = values
        if "/" in name or not hierarchy.endswith(".shier") or not animation.endswith(".sanim") or not retarget.endswith(".bin"):
            raise ValueError("Original animation profile changed format; review native selection")
        rows.append((index, *values))
    if table[1][cursor:].strip() not in ("", ",") or len(rows) != 20:
        raise ValueError("Incomplete original 20-character initializer")
    return rows


def generate(source, header):
    rows = extract(source, header)
    return "// Generated from the prepared original CharacterTemplate table.\n" + \
        "constexpr std::array<CharacterAnimationProfile, 20> character_profiles{{\n" + \
        "".join("    {" + str(row[0]) + ", " + ", ".join(json.dumps(v) for v in row[1:]) + "},\n" for row in rows) + "}};\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    content = generate(args.source.read_text(), args.header.read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != content:
        temporary = args.output.with_suffix(args.output.suffix + ".tmp")
        temporary.write_text(content)
        temporary.replace(args.output)


if __name__ == "__main__":
    main()
