#!/usr/bin/env python3
"""Make the flags beside players' names from a flag-icons package.

flag-icons (https://github.com/lipis/flag-icons, MIT) has a 4:3 SVG for every
country. This draws each ISO 3166-1 country's at CELL_WIDTH x CELL_HEIGHT
pixels into one atlas, assets/flags/flags.png, COLUMNS a row, in the order of
their codes, and writes src/overlay/flag_countries.inc: each one's code and
English name in the atlas's order (src/overlay/flags.h). Needs rsvg-convert
and ImageMagick. From the repository's root:

    curl -fsSLO https://registry.npmjs.org/flag-icons/-/flag-icons-7.2.3.tgz
    tar xzf flag-icons-7.2.3.tgz
    tools/flag_atlas.py package
"""

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

CELL_WIDTH, CELL_HEIGHT = 32, 24
COLUMNS = 16


def main(package: Path) -> None:
    out = Path("assets/flags")
    countries = [
        entry
        for entry in json.loads((package / "country.json").read_text(encoding="utf-8"))
        if entry.get("iso") and len(entry["code"]) == 2 and entry["code"].isalpha()
    ]
    countries.sort(key=lambda entry: entry["code"])
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as scratch:
        cells = []
        for entry in countries:
            cell = Path(scratch) / f"{entry['code']}.png"
            subprocess.run(
                ["rsvg-convert", "-w", str(CELL_WIDTH), "-h", str(CELL_HEIGHT),
                 "-o", str(cell), str(package / entry["flag_4x3"])],
                check=True,
            )
            cells.append(str(cell))
        subprocess.run(
            ["magick", "montage", *cells, "-tile", f"{COLUMNS}x", "-geometry",
             f"{CELL_WIDTH}x{CELL_HEIGHT}+0+0", "-background", "none", "-strip",
             f"PNG32:{out / 'flags.png'}"],
            check=True,
        )
    with open("src/overlay/flag_countries.inc", "w", encoding="utf-8", newline="\n") as table:
        table.write("// Made by tools/flag_atlas.py: the countries of assets/flags/flags.png, in its order.\n")
        for entry in countries:
            name = entry["name"].replace("\\", "\\\\").replace('"', '\\"')
            table.write(f'{{"{entry["code"].upper()}", "{name}"}},\n')
    shutil.copyfile(package / "LICENSE", out / "LICENSE-flag-icons")
    version = json.loads((package / "package.json").read_text(encoding="utf-8"))["version"]
    print(f"{len(countries)} flags from flag-icons {version} into {out}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(Path(sys.argv[1]))
