"""Portable catalogue validation and generated-resource corruption gates."""
import base64
import copy
from contextlib import closing
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("pipeline_catalog", ROOT / "tools/pipeline_catalog.py")
CATALOG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CATALOG)

# Lines the game wrote (src/runtime/pipeline_demand_log.cpp) when a boot without
# the bundled resource drew these configurations for the first time. The third
# is from a build with the earlier wording; the last is from a build before GX
# v17 and is skipped.
GAME_LOG = """Startup: graphics ready, 0 pipelines from 0 configurations in 222 ms
Graphics: new pipeline compiled while drawing in 2.7 ms: pipeline=clear5-505fbb1d66a66f52 frame=3 config=eNpjZWBgYGRkZAAAADkACQ==
Graphics: new pipeline compiled while drawing in 27.7 ms: pipeline=gx17-0e7eb3554ec81fa8 frame=66 config=eNrtls0KwjAMgPNXJ0MYHnwbX81nj+nIIIbJpu7gIYGPtmlovtJLrwCABpwMVZjjx5FZ4BLX2EjimlhkPK7f29HvRr0nvAb73gJ7HYX6yeiaZ58PRjPE5zlwZ05gf0yJIfGpgwbKoRzKoRzKoRzK4XgHTGfQSg2t9Nqq6/+Hu/EIuduf5OTLf85WcLp/W95FNT8JPgEZFmss
Graphics: drawing waited 2.4 ms to compile a new pipeline: pipeline=gx17-03cd010aba7c6d6e frame=3359 config=eNrtlksKg0AMQPOzliJIF71Nr+bZ00xJIQaLVl10kcBjZmKYPGeVOwCgARdDFd5xcGUWGOIZO5J4Jha5ndfv6+r/Rq0nzIP92wf2Ogr1o9E0r77vjc4Q3+fAjTmB7TEm+sSvDhooh3Ioh3Ioh3Ioh/MdMN1BCzW00Gutrs0PT2MKucef5GTnnLMWcV6T8F6qqunN8QUY0msq
Graphics: known pipeline compiled while drawing in 3.1 ms: pipeline=gx17-0e7eb3554ec81fa8 frame=90
Graphics: drawing waited 15.0 ms to compile a new pipeline: pipeline=gx16-d7855b8409c7cf9b frame=62 config=eNrtls0KgzAMgPNXHTJwl72Nr7ZnzzIWIQaHbnrYIYGvbWKwX+mlNwBAAzpD9Z0dnJkFrjHHRhJzYpEBT9vv4+wjGQzLYP82w95HoX80BuPi695ohvg6B+6sCeyPMdEnvnXQQDmUQzmUQzmUQzmc74DpH7TSQyt7bfW93g+T8Qi1+5/U5Md3zlZwOn+b70U1Xwk+ARvda0U=
"""


class PipelineCatalogTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "catalog.json"
        blob = bytes.fromhex("0500000001010100")
        self.catalog = {"format": 1, "schema": 1, "config_abi": CATALOG.ABI, "pipelines": [
            {"type": 0, "version": 5, "hash": "ffffffffffffffff", "first_frame": 12,
             "config": blob.hex(), "sha256": hashlib.sha256(blob).hexdigest()}]}

    def read(self, catalog=None):
        self.source.write_text(json.dumps(self.catalog if catalog is None else catalog))
        return CATALOG.read_catalog(self.source)

    def test_signed_hash_and_byte_exact_roundtrip(self):
        rows = self.read()
        self.assertEqual(rows[0][1], -1)
        target = self.root / "folder with spaces/initial_pipeline_cache.db"
        CATALOG.generate(target, rows)
        CATALOG.verify_database(target, rows)
        with closing(sqlite3.connect(target)) as db:
            self.assertEqual(db.execute("SELECT * FROM pipeline_cache").fetchall(), rows)
        self.assertFalse(Path(str(target) + "-wal").exists())

    def test_repeated_generation_is_deterministic_and_keeps_mtime(self):
        # Different flags, priorities and signed/unsigned key representations
        # make input-order mistakes observable in the SQLite rows and file bytes.
        for key, flags, frame in (("0000000000000001", "01000000", 27),
                                  ("8000000000000000", "00010100", 3)):
            blob = bytes.fromhex("05000000" + flags)
            self.catalog["pipelines"].append({
                "type": 0, "version": 5, "hash": key, "first_frame": frame,
                "config": blob.hex(), "sha256": hashlib.sha256(blob).hexdigest()})
        rows = self.read()
        shuffled = copy.deepcopy(self.catalog)
        shuffled["pipelines"].reverse()
        reordered = self.read(shuffled)
        self.assertEqual(len(rows), 3)
        self.assertEqual(rows, reordered)
        self.assertEqual([row[1] for row in rows], [1, -(1 << 63), -1])
        one, two = self.root / "one.db", self.root / "two.db"
        CATALOG.generate(one, rows)
        before = one.stat().st_mtime_ns
        CATALOG.generate(two, reordered)
        self.assertEqual(one.read_bytes(), two.read_bytes())
        CATALOG.generate(one, rows)
        self.assertEqual(one.stat().st_mtime_ns, before)

    def test_wrong_format_version_size_hash_and_endian_rejected(self):
        cases = [("version", 4), ("type", 2), ("hash", "1"), ("first_frame", -1),
                 ("first_frame", 0x100000000), ("first_frame", True),
                 ("config", "00"), ("sha256", "0" * 64)]
        for field, value in cases:
            candidate = copy.deepcopy(self.catalog)
            candidate["pipelines"][0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(CATALOG.CatalogError):
                self.read(candidate)
        for blob in (bytes.fromhex("0000000501010100"), bytes.fromhex("0500000002010100")):
            candidate = copy.deepcopy(self.catalog)
            candidate["pipelines"][0].update(config=blob.hex(), sha256=hashlib.sha256(blob).hexdigest())
            with self.assertRaises(CATALOG.CatalogError):
                self.read(candidate)
        candidate = copy.deepcopy(self.catalog)
        candidate["config_abi"] = "other"
        with self.assertRaises(CATALOG.CatalogError):
            self.read(candidate)

    def test_duplicate_and_colliding_keys_rejected(self):
        candidate = copy.deepcopy(self.catalog)
        candidate["pipelines"].append(copy.deepcopy(candidate["pipelines"][0]))
        with self.assertRaisesRegex(CATALOG.CatalogError, "duplicate/colliding"):
            self.read(candidate)
        candidate["pipelines"][1]["hash"] = "0000000000000001"
        with self.assertRaisesRegex(CATALOG.CatalogError, "duplicate configuration"):
            self.read(candidate)

    def test_duplicate_json_fields_rejected(self):
        self.source.write_text('{"format":1,"format":1}')
        with self.assertRaisesRegex(CATALOG.CatalogError, "Duplicate JSON"):
            CATALOG.read_catalog(self.source)

    def test_corrupt_and_stale_databases_rejected(self):
        rows = self.read()
        target = self.root / "resource.db"
        target.write_bytes(b"not sqlite")
        with self.assertRaises(CATALOG.CatalogError):
            CATALOG.verify_database(target, rows)
        CATALOG.generate(target, rows)
        with closing(sqlite3.connect(target)) as db:
            db.execute("UPDATE pipeline_cache SET first_frame_used=13")
            db.commit()
        with self.assertRaisesRegex(CATALOG.CatalogError, "differs"):
            CATALOG.verify_database(target, rows)
        CATALOG.generate(target, rows)
        with closing(sqlite3.connect(target)) as db:
            db.execute("CREATE TABLE private_data(value BLOB)")
            db.commit()
        with self.assertRaisesRegex(CATALOG.CatalogError, "Unexpected tables"):
            CATALOG.verify_database(target, rows)

    def test_symlinks_rejected_and_failed_validation_keeps_output(self):
        rows = self.read()
        real = self.root / "real.db"
        CATALOG.generate(real, rows)
        link = self.root / "link.db"
        link.symlink_to(real)
        before = real.read_bytes()
        with self.assertRaises(CATALOG.CatalogError):
            CATALOG.generate(link, rows)
        with self.assertRaises(CATALOG.CatalogError):
            CATALOG.verify_database(link, rows)
        self.assertEqual(before, real.read_bytes())

    def test_import_merges_recorded_rows(self):
        self.read()
        runtime = self.root / "pipeline_cache.db"
        known = bytes.fromhex("0500000001010100")
        new = bytes.fromhex("0500000000010000")
        with closing(sqlite3.connect(runtime)) as db:
            db.executescript(CATALOG.TABLE_SQL)
            db.executemany("INSERT INTO pipeline_cache VALUES (?,?,?,?,?,?)", [
                (0, -1, 5, 8, known, 3),           # Already reviewed; earlier first use.
                (0, 2, 5, 8, new, 40),             # New configuration.
                (1, 3, 15, 2772, bytes(2772), 1),  # Obsolete version: skipped.
            ])
            db.commit()
        before = runtime.read_bytes()
        self.assertEqual(CATALOG.import_databases(self.source, [runtime]), 1)
        self.assertEqual(runtime.read_bytes(), before)
        rows = CATALOG.read_catalog(self.source)
        self.assertEqual([(row[1], row[5]) for row in rows], [(2, 40), (-1, 3)])
        self.assertEqual(CATALOG.import_databases(self.source, [runtime]), 0)
        with closing(sqlite3.connect(runtime)) as db:
            db.execute("UPDATE pipeline_cache SET config=? WHERE hash=2", (known,))
            db.commit()
        with self.assertRaisesRegex(CATALOG.CatalogError, "two configurations"):
            CATALOG.import_databases(self.source, [runtime])

    def test_xxh3_matches_every_recorded_key(self):
        rows = CATALOG.read_catalog(ROOT / "assets/graphics/pipelines.json")
        self.assertEqual({row[0] for row in rows}, {0, CATALOG.GX})
        for kind, key, _, _, blob, _ in rows:
            self.assertEqual(CATALOG.xxh3_64(blob, kind), key & 0xFFFFFFFFFFFFFFFF)
        for size in (3, 9, 240):
            with self.assertRaises(CATALOG.CatalogError):
                CATALOG.xxh3_64(bytes(size), CATALOG.GX)

    def gx_row(self, marker, frame):
        blob = bytearray(2772)
        blob[0] = 17
        blob[12] = marker  # Any field: tells the synthetic configurations apart.
        key = CATALOG.xxh3_64(bytes(blob), CATALOG.GX)
        return {"type": 1, "version": 17, "hash": f"{key:016x}", "first_frame": frame,
                "config": bytes(blob).hex(), "sha256": hashlib.sha256(blob).hexdigest()}

    def log_line(self, row, frame=7, encoded=None, name="gx"):
        blob = bytes.fromhex(row["config"])
        if encoded is None:
            encoded = base64.b64encode(zlib.compress(blob, 9)).decode()
        return (f"Graphics: new pipeline compiled while drawing in 4.0 ms: "
                f"pipeline={name}{row['version']}-{row['hash']} frame={frame} config={encoded}\n")

    def test_game_log_lines_hold_the_catalogue_configurations(self):
        log = self.root / "mscharged.log"
        log.write_text(GAME_LOG)
        rows = CATALOG.read_log(log)
        self.assertEqual([(row[0], row[1] & 0xFFFFFFFFFFFFFFFF, row[5]) for row in rows],
                         [(0, 0x505fbb1d66a66f52, 3), (CATALOG.GX, 0x0e7eb3554ec81fa8, 66),
                          (CATALOG.GX, 0x03cd010aba7c6d6e, 3359)])
        catalogue = {(row[0], row[1]): row[4] for row in CATALOG.read_catalog(ROOT / "assets/graphics/pipelines.json")}
        for kind, key, version, size, blob, _ in rows:
            self.assertEqual(catalogue[(kind, key)], blob)
            self.assertEqual((version, size), CATALOG.LAYOUTS[kind])
        fresh = self.root / "fresh.json"
        self.assertEqual(CATALOG.import_databases(fresh, [log]), 3)
        self.assertEqual(len(CATALOG.read_catalog(fresh)), 3)

    def test_import_reads_session_logs(self):
        gx = self.gx_row(9, 0)
        log = self.root / "mscharged.log"
        log.write_text("=== Mario Strikers Charged native port ===\n"
                       "Graphics: known pipeline compiled while drawing in 1.0 ms: pipeline=gx17-0123456789abcdef frame=4\n"
                       + self.log_line(gx, frame=812)
                       # Another renderer version: skipped, as in a runtime cache.
                       + self.log_line(dict(gx, version=16), frame=3))
        self.read()
        self.assertEqual(CATALOG.import_databases(self.source, [log]), 1)
        rows = {row[1] & 0xFFFFFFFFFFFFFFFF: row for row in CATALOG.read_catalog(self.source)}
        self.assertEqual(rows[int(gx["hash"], 16)][4], bytes.fromhex(gx["config"]))
        self.assertEqual(rows[int(gx["hash"], 16)][5], 812)
        self.assertEqual(CATALOG.import_databases(self.source, [log]), 0)

    def test_damaged_log_lines_rejected(self):
        gx = self.gx_row(9, 0)
        blob = bytes.fromhex(gx["config"])
        encoded = base64.b64encode(zlib.compress(blob, 9)).decode()
        other = self.gx_row(10, 0)
        cases = {
            "unreadable configuration": [encoded[:-6] + "*" + encoded[-5:], base64.b64encode(b"not zlib").decode()],
            "truncated": [encoded[:-8]],
            "wrong size": [base64.b64encode(zlib.compress(blob[:-1])).decode(),
                           base64.b64encode(zlib.compress(blob + b"\0")).decode(),
                           base64.b64encode(zlib.compress(blob) + b"tail").decode()],
            "does not match its key": [base64.b64encode(zlib.compress(bytes.fromhex(other["config"]))).decode()],
        }
        self.read()
        before = self.source.read_bytes()
        log = self.root / "mscharged.log"
        for message, encodings in cases.items():
            for value in encodings:
                log.write_text("first line\n" + self.log_line(gx, encoded=value))
                with self.subTest(value=value), self.assertRaisesRegex(CATALOG.CatalogError, f"mscharged.log:2: .*{message}"):
                    CATALOG.import_databases(self.source, [log])
        for line in ("Graphics: new pipeline compiled while drawing in 1.0 ms: pipeline=gx17-0123 frame=1 config=AAAA\n",
                     "Graphics: new pipeline compiled while drawing in 1.0 ms: pipeline=gx17-0123456789abcdef config=AAAA\n",
                     self.log_line(gx).replace("\n", " trailing\n")):
            log.write_text(line)
            with self.subTest(line=line), self.assertRaisesRegex(CATALOG.CatalogError, "unreadable pipeline line"):
                CATALOG.import_databases(self.source, [log])
        self.assertEqual(self.source.read_bytes(), before)

    def test_reviewed_catalogue_generates_and_verifies(self):
        recorded = CATALOG.read_catalog(ROOT / "assets/graphics/pipelines.json")
        self.assertTrue(any(row[0] == 0 for row in recorded))
        self.assertTrue(any(row[0] == 1 for row in recorded))
        rows = CATALOG.catalog_rows(ROOT / "assets/graphics/pipelines.json")
        self.assertEqual(rows, recorded)
        self.assertEqual(len({(row[0], row[1]) for row in rows}), len(rows))
        target = self.root / "initial_pipeline_cache.db"
        CATALOG.generate(target, rows)
        CATALOG.verify_database(target, rows)


if __name__ == "__main__":
    unittest.main()
