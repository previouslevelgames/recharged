#!/usr/bin/env python3
"""Fail-closed source-table extraction, including unused-field drift."""
import importlib.util
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("profiles", root / "tools/extract_character_animation_profiles.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
prepared = Path(sys.argv[1])
source = (prepared / "src/Game/CharacterTemplate.cpp").read_text()
header = (prepared / "include/Game/CharacterTemplate.h").read_text()
rows = module.extract(source, header)
assert len(rows) == 20 and tuple(row[0] for row in rows) == tuple(range(20))
assert rows[0] == (0, "mario", "art/animation/mario.shier", "art/animation/mariofe.sanim", "art/animation/mario/animretarget/mario.bin")
assert rows[-1][1] == "shyguy"
changed = module.extract(source.replace('"art/animation/mariofe.sanim"', '"art/animation/reviewedfe.sanim"'), header)
assert changed[0][3] == "art/animation/reviewedfe.sanim" and changed[1:] == rows[1:]
for bad_source, bad_header in (
    (source.replace("g_aCharacterTemplateInfo[20]", "g_aCharacterTemplateInfo[21]"), header),
    (source.replace("(eCharacterClass)0,", "(eCharacterClass)1,"), header),
    (source.replace('"art/animation/mariofe.sanim",', 'nullptr,'), header),
    (source.replace('"art/animation/mariofe.sanim",', '"art/animation/mariofe.cam",'), header),
    (source.replace('"art/animation/mario.shier",', '"art/animation/mario.shier", 19,'), header),
    (source.replace("CharacterAnimTriggerCallback,", "InventedCallback(),"), header),
    (source.replace("GLOBALAnimProperties,", "Unknown + 1,"), header),
    (source.replace("    },\n    {", "    },,\n    {", 1), header),
    (source.replace("    },\n    {", "    }\n    {", 1), header),
    (source.replace('"art/animation/mariofe.sanim",', '"art/animation/mariofe.sanim" + suffix,'), header),
    (source, header.replace("szHierarchyFilename;", "newHierarchyField;")),
    (source, header.replace("int nNumAnimProperties;", "int nNumAnimProperties; int newField;")),
):
    assert bad_source != source or bad_header != header, "The drift fixture did not alter the reviewed source"
    try:
        module.extract(bad_source, bad_header)
    except (ValueError, TypeError):
        pass
    else:
        raise AssertionError("Changed original schema/initializer accepted")
print("16 original character profile extraction checks passed")
