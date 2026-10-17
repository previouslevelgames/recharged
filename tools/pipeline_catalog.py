#!/usr/bin/env python3
"""Generate, verify or extend Aurora's portable pipeline descriptions, never driver code.

The reviewed source pins each recorded XXH3 key and configuration SHA-256.
--import merges configurations that the game recorded in runtime caches, or
that a session log (logs/recharged.log) lists because a draw had to compile
them; Aurora derives a configuration's key from its bytes again whenever it
loads a row. Fog and vertex array byte order are per-draw state (GX v17), so
one recorded configuration covers every stadium's fog. Ordinary builds require
only the Python standard library and never inspect local runtime caches. The
bundled format is for the supported little-endian 64-bit hosts.
"""

import argparse
import base64
from contextlib import closing
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sqlite3
import struct
import tempfile
import zlib

ABI = "aurora-gx17-clear5-le64"
LAYOUTS = {0: (5, 8), 1: (17, 2772)}
GX = 1
SQLITE_HEADER = b"SQLite format 3\0"
# A session log line for a draw that compiled a pipeline new on that PC, written
# by src/runtime/pipeline_demand_log.cpp: the configuration is base64 of zlib.
LOG_ENTRY = re.compile(r"\bpipeline=(gx|clear)(\d+)-([0-9a-f]{16}) frame=(\d+) config=(\S+)$")
LOG_TYPES = {"clear": 0, "gx": GX}
_M64 = (1 << 64) - 1
# xxHash 0.8 XXH3 default secret (XXH3_kSecret).
_XXH3_SECRET = bytes.fromhex(
    "b8fe6c3923a44bbe7c01812cf721ad1cded46de9839097db7240a4a4b7b3671fcb79e64eccc0e578825ad07dccff7221"
    "b8084674f743248ee03590e6813a264c3c2852bb91c300cb88d0658b1b532ea371644897a20df94e3819ef46a9deacd8"
    "a8fa763fe39c343ff9dcbbc7c70b4f1d8a51e04bcdb45931c89f7ec9d9787364eac5ac8334d3ebc3c581a0fffa1363eb"
    "170ddd51b7f0da49d316552629d4689e2b16be587d47a1fc8ff8b8d17ad031ce45cb3a8f95160428afd7fbcabb4b407e")
TABLE_SQL = """CREATE TABLE aurora_schema(value INTEGER);
INSERT INTO aurora_schema VALUES (1);
CREATE TABLE pipeline_cache (
  type INTEGER NOT NULL,
  hash INTEGER NOT NULL,
  config_version INTEGER NOT NULL,
  config_size INTEGER NOT NULL,
  config BLOB NOT NULL,
  first_frame_used INTEGER NOT NULL,
  PRIMARY KEY (type, hash)
);
CREATE INDEX pipeline_cache_load_order_idx
  ON pipeline_cache(type, config_version, first_frame_used);
"""


class CatalogError(ValueError):
    pass


def _integer(value, low, high):
    return type(value) is int and low <= value <= high


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise CatalogError(f"Duplicate JSON field: {key}")
        result[key] = value
    return result


def read_catalog(path):
    """Validate reviewed inputs and return stable SQLite tuples."""
    try:
        catalog = json.loads(Path(path).read_text(encoding="utf-8"), object_pairs_hook=_object)
    except (OSError, ValueError) as error:
        raise CatalogError(f"Cannot read pipeline catalogue: {error}") from error
    if (not isinstance(catalog, dict)
            or set(catalog) != {"format", "schema", "config_abi", "pipelines"}
            or type(catalog["format"]) is not int or catalog["format"] != 1
            or type(catalog["schema"]) is not int or catalog["schema"] != 1
            or catalog["config_abi"] != ABI):
        raise CatalogError("Unsupported pipeline catalogue format, schema or ABI")
    if not isinstance(catalog["pipelines"], list) or not 1 <= len(catalog["pipelines"]) <= 100000:
        raise CatalogError("Expected a nonempty, bounded pipeline list")
    rows, keys, configs = [], set(), set()
    for index, row in enumerate(catalog["pipelines"]):
        prefix = f"Pipeline {index}: "
        if not isinstance(row, dict) or set(row) != {"type", "hash", "version", "first_frame", "config", "sha256"}:
            raise CatalogError(prefix + "invalid fields")
        kind = row["type"]
        if type(kind) is not int or kind not in LAYOUTS:
            raise CatalogError(prefix + "unsupported shader type")
        version, size = LAYOUTS[kind]
        if type(row["version"]) is not int or row["version"] != version:
            raise CatalogError(prefix + "unsupported configuration version")
        if not _integer(row["first_frame"], 0, 0xFFFFFFFF):
            raise CatalogError(prefix + "invalid first-use frame")
        if not isinstance(row["hash"], str) or not re.fullmatch(r"[0-9a-f]{16}", row["hash"]):
            raise CatalogError(prefix + "invalid captured XXH3 key")
        if not isinstance(row["config"], str) or not re.fullmatch(r"[0-9a-f]{" + str(size * 2) + "}", row["config"]):
            raise CatalogError(prefix + "invalid configuration encoding/size")
        blob = bytes.fromhex(row["config"])
        if row["sha256"] != hashlib.sha256(blob).hexdigest():
            raise CatalogError(prefix + "configuration SHA-256 mismatch")
        if struct.unpack_from("<I", blob)[0] != version:
            raise CatalogError(prefix + "configuration has wrong byte order/version")
        if kind == 0:
            if any(value > 1 for value in blob[4:7]) or blob[7] != 0:
                raise CatalogError(prefix + "invalid clear flags/padding")
        else:
            # These stable tail fields also catch a wrong structure layout.
            enums = struct.unpack_from("<6I", blob, size - 44)
            if any(value > limit for value, limit in zip(enums, (7, 3, 3, 7, 7, 15))):
                raise CatalogError(prefix + "invalid GX fixed-function enum")
            alpha = struct.unpack_from("<I", blob, size - 20)[0]
            if alpha > 255 and alpha != 0xFFFFFFFF:
                raise CatalogError(prefix + "invalid destination alpha")
            if any(value > 1 for value in blob[-4:]):
                raise CatalogError(prefix + "invalid GX flags")
            if not all(math.isfinite(value) for value in struct.unpack_from("<3f", blob, size - 16)):
                raise CatalogError(prefix + "invalid polygon offset")
        key = (kind, int(row["hash"], 16))
        if key in keys:
            raise CatalogError(prefix + "duplicate/colliding pipeline key")
        if (kind, blob) in configs:
            raise CatalogError(prefix + "duplicate configuration under different keys")
        keys.add(key)
        configs.add((kind, blob))
        signed_hash = key[1] if key[1] < (1 << 63) else key[1] - (1 << 64)
        rows.append((kind, signed_hash, version, size, blob, row["first_frame"]))
    return sorted(rows, key=lambda r: (r[0], r[1] & 0xFFFFFFFFFFFFFFFF))


def xxh3_64(data, seed):
    """XXH3_64bits_withSeed, as Aurora keys a configuration: the scalar paths of
    xxHash 0.8 for 4 to 8 bytes (clear) and more than 240 bytes (GX)."""
    length = len(data)
    if 4 <= length <= 8:
        seed = (seed ^ (int.from_bytes(struct.pack("<I", seed & 0xFFFFFFFF), "big") << 32)) & _M64
        low, high = struct.unpack_from("<I", data)[0], struct.unpack_from("<I", data, length - 4)[0]
        bitflip = ((struct.unpack_from("<Q", _XXH3_SECRET, 8)[0] ^ struct.unpack_from("<Q", _XXH3_SECRET, 16)[0])
                   - seed) & _M64
        h = (high + (low << 32)) ^ bitflip
        h ^= ((h << 49 | h >> 15) ^ (h << 24 | h >> 40)) & _M64
        h = (h * 0x9FB21C651E98DF25) & _M64
        h ^= (h >> 35) + length
        h = (h * 0x9FB21C651E98DF25) & _M64
        return h ^ (h >> 28)
    if length <= 240:
        raise CatalogError("XXH3 is implemented only for inputs of 4 to 8 or more than 240 bytes")
    words = struct.unpack("<24Q", _XXH3_SECRET)
    secret = struct.pack("<24Q", *(((word + seed) if i % 2 == 0 else (word - seed)) & _M64
                                   for i, word in enumerate(words)))
    acc = [0xC2B2AE3D, 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9,
           0x85EBCA77C2B2AE63, 0x85EBCA77, 0x27D4EB2F165667C5, 0x9E3779B1]

    def accumulate(offset, secret_offset):
        values = struct.unpack_from("<8Q", data, offset)
        keys = struct.unpack_from("<8Q", secret, secret_offset)
        for lane in range(8):
            key = values[lane] ^ keys[lane]
            acc[lane ^ 1] = (acc[lane ^ 1] + values[lane]) & _M64
            acc[lane] = (acc[lane] + (key & 0xFFFFFFFF) * (key >> 32)) & _M64

    stripes = (len(secret) - 64) // 8
    block = 64 * stripes
    blocks = (length - 1) // block
    scramble = struct.unpack_from("<8Q", secret, len(secret) - 64)
    for n in range(blocks):
        for stripe in range(stripes):
            accumulate(n * block + stripe * 64, stripe * 8)
        for lane in range(8):
            acc[lane] = ((acc[lane] ^ (acc[lane] >> 47) ^ scramble[lane]) * 0x9E3779B1) & _M64
    for stripe in range((length - 1 - block * blocks) // 64):
        accumulate(blocks * block + stripe * 64, stripe * 8)
    accumulate(length - 64, len(secret) - 64 - 7)
    result = (length * 0x9E3779B185EBCA87) & _M64
    for i in range(4):
        low, high = struct.unpack_from("<2Q", secret, 11 + 16 * i)
        product = (acc[2 * i] ^ low) * (acc[2 * i + 1] ^ high)
        result = (result + ((product & _M64) ^ (product >> 64))) & _M64
    result ^= result >> 37
    result = (result * 0x165667919E3779F9) & _M64
    return result ^ (result >> 32)


def catalog_rows(path):
    """The rows of the generated resource."""
    return read_catalog(path)


def verify_database(path, rows):
    """Reject missing/stale/modified package resources without modifying them."""
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise CatalogError(f"Missing or unsafe pipeline resource: {path}")
    try:
        with closing(sqlite3.connect(path.resolve().as_uri() + "?mode=ro&immutable=1", uri=True)) as db:
            if db.execute("PRAGMA integrity_check").fetchall() != [("ok",)]:
                raise CatalogError("Pipeline resource failed SQLite integrity check")
            tables = db.execute("SELECT name FROM sqlite_schema WHERE type='table' ORDER BY name").fetchall()
            if tables != [("aurora_schema",), ("pipeline_cache",)]:
                raise CatalogError("Unexpected tables in pipeline resource")
            if db.execute("SELECT value FROM aurora_schema").fetchall() != [(1,)]:
                raise CatalogError("Wrong pipeline resource schema version")
            actual = db.execute("SELECT type,hash,config_version,config_size,config,first_frame_used FROM pipeline_cache").fetchall()
            actual.sort(key=lambda r: (r[0], r[1] & 0xFFFFFFFFFFFFFFFF))
            if actual != rows:
                raise CatalogError("Pipeline resource differs from its reviewed catalogue")
    except (sqlite3.Error, TypeError) as error:
        raise CatalogError(f"Invalid pipeline resource: {error}") from error


def generate(path, rows):
    """Write a standalone, deterministic DB atomically; keep unchanged mtimes."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".pipeline-catalog-", dir=path.parent) as temporary:
        candidate = Path(temporary) / "initial_pipeline_cache.db"
        with closing(sqlite3.connect(candidate)) as db:
            db.executescript("PRAGMA page_size=4096; PRAGMA journal_mode=DELETE;" + TABLE_SQL)
            db.executemany("INSERT INTO pipeline_cache VALUES (?,?,?,?,?,?)", rows)
            db.commit()
            db.execute("VACUUM")
        verify_database(candidate, rows)
        data = candidate.read_bytes()
        if path.is_symlink():
            raise CatalogError(f"Refusing symlink output: {path}")
        if path.is_file() and path.read_bytes() == data:
            return
        candidate.chmod(0o644)
        os.replace(candidate, path)


def read_log(path):
    """The configurations a session log lists: one line for each pipeline a
    draw compiled because it was new on that PC. Rows of other configuration
    versions are skipped; a damaged line is an error."""
    rows = []
    with open(path, encoding="utf-8", errors="replace") as log:
        for number, line in enumerate(log, 1):
            if "pipeline=" not in line or " config=" not in line:
                continue  # Other lines, and known configurations (no config).
            where = f"{path}:{number}: "
            match = LOG_ENTRY.search(line.rstrip())
            if not match:
                raise CatalogError(where + "unreadable pipeline line")
            name, version, key, frame, encoded = match.groups()
            kind = LOG_TYPES[name]
            if LAYOUTS[kind][0] != int(version):
                continue  # Another renderer's or an obsolete configuration.
            size = LAYOUTS[kind][1]
            try:
                inflater = zlib.decompressobj()
                blob = inflater.decompress(base64.b64decode(encoded, validate=True), size + 1)
            except (ValueError, zlib.error) as error:
                raise CatalogError(where + f"unreadable configuration ({error})") from error
            if len(blob) > size or (inflater.eof and (len(blob) != size or inflater.unused_data)):
                raise CatalogError(where + "configuration has the wrong size")
            if not inflater.eof:
                raise CatalogError(where + "configuration is truncated")
            if xxh3_64(blob, kind) != int(key, 16):
                raise CatalogError(where + "configuration does not match its key")
            if not _integer(int(frame), 0, 0xFFFFFFFF):
                raise CatalogError(where + "invalid frame")
            signed_key = int(key, 16) - (1 << 64) if int(key, 16) >= 1 << 63 else int(key, 16)
            rows.append((kind, signed_key, int(version), size, blob, int(frame)))
    return rows


def read_recording(path):
    """Rows of a runtime pipeline_cache.db or of a session log."""
    path = Path(path)
    if not path.is_file():
        raise CatalogError(f"Missing pipeline cache or log: {path}")
    with open(path, "rb") as file:
        if file.read(len(SQLITE_HEADER)) != SQLITE_HEADER:
            return read_log(path)
    try:
        with closing(sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True)) as db:
            return db.execute("SELECT type,hash,config_version,config_size,config,first_frame_used "
                              "FROM pipeline_cache").fetchall()
    except sqlite3.Error as error:
        raise CatalogError(f"Cannot read runtime pipeline cache {path}: {error}") from error


def import_databases(catalog, databases):
    """Merge the game's recorded configurations into the reviewed catalogue.

    Reads runtime caches and session logs without changing them and keeps only
    rows of the catalogue's configuration versions. Returns the number of new
    entries."""
    catalog = Path(catalog)
    if catalog.exists():
        read_catalog(catalog)
        document = json.loads(catalog.read_text(encoding="utf-8"))
    else:
        document = {"format": 1, "schema": 1, "config_abi": ABI, "pipelines": []}
    entries = {(row["type"], row["hash"]): row for row in document["pipelines"]}
    before = len(entries)
    for database in databases:
        for kind, key, version, size, blob, frame in read_recording(database):
            if LAYOUTS.get(kind) != (version, size) or not isinstance(blob, bytes):
                continue  # Another renderer's or an obsolete configuration.
            entry = {"type": kind, "hash": f"{key & 0xFFFFFFFFFFFFFFFF:016x}", "version": version,
                     "first_frame": frame, "config": blob.hex(), "sha256": hashlib.sha256(blob).hexdigest()}
            existing = entries.setdefault((kind, entry["hash"]), entry)
            if existing["config"] != entry["config"]:
                raise CatalogError(f"Recorded key {entry['hash']} names two configurations")
            existing["first_frame"] = min(existing["first_frame"], frame)
    document["pipelines"] = [entries[key] for key in sorted(entries)]
    text = json.dumps(document, indent=2, separators=(",", ":")) + "\n"
    with tempfile.TemporaryDirectory(prefix=".pipeline-catalog-", dir=catalog.parent) as temporary:
        candidate = Path(temporary) / catalog.name
        candidate.write_text(text, encoding="utf-8")
        read_catalog(candidate)
        candidate.chmod(0o644)
        os.replace(candidate, catalog)
    return len(entries) - before


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, required=True)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--output", type=Path)
    action.add_argument("--verify", type=Path)
    action.add_argument("--import", dest="databases", type=Path, nargs="+", metavar="CACHE_OR_LOG",
                        help="merge configurations recorded in runtime pipeline_cache.db files or "
                             "listed in session logs (logs/recharged.log)")
    args = parser.parse_args()
    try:
        if args.databases:
            added = import_databases(args.catalog, args.databases)
            print(f"Pipeline catalogue: {added} new configurations imported")
        rows = catalog_rows(args.catalog)
        if args.output:
            generate(args.output, rows)
        elif args.verify:
            verify_database(args.verify, rows)
    except (CatalogError, OSError) as error:
        parser.exit(1, f"pipeline catalogue: {error}\n")
    print(f"Pipeline catalogue: {len(rows)} verified configurations")


if __name__ == "__main__":
    main()
