#!/usr/bin/env python3
"""Build assets/gameplay/robot_goalie.map from both games' files (see assets/gameplay/README.md).

The map tells, for each texel of Charged's goalie textures, where its colour
comes from on the GameCube robot goalie texture, which Charged's own disc keeps
unused (art/characters/superteamgoalie). It needs the extracted file systems of
both games' discs (for example `nodtool extract DISC DIR`, then DIR/files):

    python3 tools/robot_goalie/make_map.py --charged CHARGED/files \\
        --gamecube STRIKERS/files --out assets/gameplay/robot_goalie.map

--preview DIR also writes the textures the map makes from Charged's leftover
texture as PNG files. A C++17 compiler ($CXX, else c++) builds the mapper.
Game files and previews stay out of the repository.
"""

import argparse
import colorsys
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
import models  # noqa: E402

# Charged packets that bind these textures are mapped: the goalie kit (every
# team's kit and away kit share the layout of Mario's) and the eye.
TARGETS = [("mariogoalie/mariogoalie", 256, 256), (0xFE561F0E, 128, 64)]


def find(root, relative):
    """A path below root, matched without regard to case (Charged uses Art/, the GameCube art/)."""
    path = Path(root)
    for part in Path(relative).parts:
        matches = [p for p in path.iterdir() if p.name.lower() == part.lower()] if path.is_dir() else []
        if not matches:
            raise SystemExit(f"missing {relative} below {root}")
        path = matches[0]
    return path


# The robot's crosshair lens on its texture (s0, t0, s1, t1). Charged's eyeball is a white
# ball with a red slit under a green lid; the robot's eyes are red lenses, so its eyeball
# gets this lens instead of whatever head plating lies nearest.
LENS = (0.52, 0.075, 0.68, 0.175)
EYE = 0xFE561F0E


def eyeball(width, height, rgba):
    """The eyeball's texels in Charged's eye texture: not the green lid or skin, rows filled in."""
    inside = []
    for i in range(width * height):
        h, s, v = colorsys.rgb_to_hsv(*(c / 255 for c in rgba[i * 4:i * 4 + 3]))
        inside.append(not (0.15 < h < 0.45 and s > 0.2) and v >= 0.12)
    for y in range(height):  # the slit's dark edges belong to it
        row = [x for x in range(width) if inside[y * width + x]]
        for x in range(row[0], row[-1] + 1) if row else ():
            inside[y * width + x] = True
    return inside


def place_lens(path, inside):
    """Rewrites the eye target of a map file: eyeball texels sample the lens."""
    data = bytearray(Path(path).read_bytes())
    count = struct.unpack_from("<I", data, 12)[0]
    offset, out = 16, data[:16]
    for _ in range(count):
        texture, width, height, samples = struct.unpack_from("<IHHI", data, offset)
        counts = list(data[offset + 12:offset + 12 + width * height])
        pos = offset + 12 + width * height
        texels = []
        for n in counts:
            texels.append([struct.unpack_from("<HH", data, pos + 4 * k) for k in range(n)])
            pos += 4 * n
        if texture == EYE:
            xs = [i % width for i in range(width * height) if inside[i]]
            ys = [i // width for i in range(width * height) if inside[i]]
            x0, x1, y0, y1 = min(xs), max(xs) + 1, min(ys), max(ys) + 1
            for i in range(width * height):
                if inside[i]:
                    s = LENS[0] + ((i % width) + 0.5 - x0) / (x1 - x0) * (LENS[2] - LENS[0])
                    t = LENS[1] + ((i // width) + 0.5 - y0) / (y1 - y0) * (LENS[3] - LENS[1])
                    texels[i] = [(round(s * 65535), round(t * 65535))]
        out += struct.pack("<IHHI", texture, width, height, sum(len(t) for t in texels))
        out += bytes(len(t) for t in texels)
        for t in texels:
            for sample in t:
                out += struct.pack("<HH", *sample)
        offset = pos
    Path(path).write_bytes(bytes(out))


def write_png(path, width, height, rgba):
    rows = b"".join(b"\0" + bytes(rgba[y * width * 4:(y + 1) * width * 4]) for y in range(height))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--charged", required=True, help="files/ of an extracted Mario Strikers Charged disc")
    parser.add_argument("--gamecube", required=True, help="files/ of an extracted Super Mario Strikers disc")
    parser.add_argument("--out", required=True, help="the map to write")
    parser.add_argument("--preview", help="directory for PNG previews of the made textures")
    args = parser.parse_args()

    charged = models.read_wii_model(find(args.charged, "Art/characters/mariogoalie/mariogoalie.rlg"))
    charged_parents = models.read_parents(find(args.charged, "Art/animation/mariogoalie.shier"))
    gamecube = models.read_gamecube_model(find(args.gamecube, "art/characters/MarioGoalie/MarioGoalie.glg"))
    gamecube_parents = models.read_parents(find(args.gamecube, "art/animation/MarioGoalie.shier"))

    with tempfile.TemporaryDirectory(prefix="robot-goalie-") as work:
        work = Path(work)
        models.write_mesh(work / "gamecube.msh", gamecube, gamecube_parents)
        models.write_mesh(work / "charged.msh", charged, charged_parents)
        compiler = os.environ.get("CXX") or shutil.which("c++") or "g++"
        mapper = work / "make_map"
        subprocess.run([compiler, "-O2", "-std=c++17", "-o", str(mapper), str(Path(__file__).with_name("make_map.cpp"))],
                       check=True)
        command = [str(mapper), str(work / "gamecube.msh"), str(work / "charged.msh"), args.out]
        for texture, width, height in TARGETS:
            hash_ = models.name_hash(texture) if isinstance(texture, str) else texture
            command += ["--target", f"{hash_:08x}:{width}:{height}"]
        if args.preview:
            preview = Path(args.preview)
            preview.mkdir(parents=True, exist_ok=True)
            w, h, rgba = models.read_texture(find(args.charged, "Art/characters/superteamgoalie/superteamgoalie.rlt"),
                                             models.name_hash("superteamgoalie/superteamgoalie"))
            (work / "source.rgba").write_bytes(struct.pack("<II", w, h) + bytes(rgba))
        subprocess.run(command, check=True)
        w, h, eye = models.read_texture(find(args.charged, "Art/characters/mariogoalie/mariogoalie.rlt"), EYE)
        place_lens(args.out, eyeball(w, h, eye))
        if args.preview:
            subprocess.run([str(mapper), "--apply", args.out, str(work / "source.rgba"), str(work / "made_")], check=True)
            for made in sorted(work.glob("made_*.rgba")):
                data = made.read_bytes()
                w, h = struct.unpack_from("<II", data)
                write_png(preview / (made.stem + ".png"), w, h, data[8:])


if __name__ == "__main__":
    main()
