"""Synthetic source-ordered particle file bytes; contains no game data."""
import struct
import zlib

PATHS = ("Art/effects/effects.bun", "Art/effects/effectsNonRes.bun.zlib",
         "Art/objects/effectsgeometry.bun", "Art/objects/effectsgeometrytextures.rlt")


def particle_files_fixture():
    raw = [bytes([index + 1]) * (101 + index * 17) for index in range(4)]
    files = dict(zip(PATHS, raw))
    files[PATHS[1]] = struct.pack(">I", len(raw[1])) + zlib.compress(raw[1])
    return files
