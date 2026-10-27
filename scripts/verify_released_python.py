#!/usr/bin/env python3
"""Cross-platform smoke test for the RELEASED neug Python SDK.

The CI workflow pip-installs the published wheel (`neug==<version>`) and runs
this script from a neutral CWD so `import neug` resolves to site-packages, not
the repo source tree (tools/python_bind/neug), which has no compiled binding.

It verifies three things and exits non-zero if any fails:
  1) import + version match EXPECT_VERSION (default 0.3.0)
  2) basic create/insert/query on an in-memory database
  3) INSTALL + LOAD each of the 6 official extensions (downloaded from the
     release OSS bucket for the current platform)

Prints a machine-readable summary: BASE_RESULT / EXT_MATRIX / EXT_ALL_OK / FINAL.
"""
import os
import sys
import platform

EXPECTED = os.environ.get("EXPECT_VERSION", "0.3.0")
EXTENSIONS = ["parquet", "httpfs", "pattern_matching", "vector_search", "fts", "gds"]


def log(msg=""):
    print(msg, flush=True)


base_fail = 0


def check(name, fn):
    global base_fail
    try:
        fn()
        log(f"  [PASS] {name}")
        return True
    except Exception as e:  # noqa: BLE001
        log(f"  [FAIL] {name}: {type(e).__name__}: {e}")
        base_fail += 1
        return False


log("=== 0. environment ===")
log(f"  python   : {sys.version.split()[0]} ({platform.machine()})")
log(f"  platform : {platform.platform()}")

try:
    import neug
    from neug.database import Database
except Exception as e:  # noqa: BLE001
    log(f"  [FAIL] import neug: {type(e).__name__}: {e}")
    log("BASE_RESULT: FAIL")
    log("FINAL: FAIL")
    sys.exit(2)

log(f"  neug.__version__ = {neug.__version__}")
ver_ok = neug.__version__ == EXPECTED
log(f"  [{'PASS' if ver_ok else 'FAIL'}] version == {EXPECTED}")
if not ver_ok:
    base_fail += 1

log("\n=== 1. basic insert / query (in-memory) ===")
db = Database("")
conn = db.connect()


def _schema():
    conn.execute("CREATE NODE TABLE Person(id INT64 PRIMARY KEY, name STRING, age INT64)")
    conn.execute("CREATE NODE TABLE Company(id INT64 PRIMARY KEY, name STRING)")
    conn.execute("CREATE REL TABLE WORKS_FOR(FROM Person TO Company, salary DOUBLE)")
    conn.execute("CREATE REL TABLE KNOWS(FROM Person TO Person, since INT64)")


def _insert():
    conn.execute("CREATE (:Person {id: 1, name: 'Alice', age: 30})")
    conn.execute("CREATE (:Person {id: 2, name: 'Bob', age: 35})")
    conn.execute("CREATE (:Company {id: 1, name: 'TechCorp'})")
    conn.execute("MATCH (p:Person {id:1}),(c:Company {id:1}) "
                 "CREATE (p)-[:WORKS_FOR {salary: 75000.0}]->(c)")
    conn.execute("MATCH (a:Person {id:2}),(b:Person {id:1}) CREATE (a)-[:KNOWS {since: 2018}]->(b)")


def _q_persons():
    rows = list(conn.execute("MATCH (p:Person) RETURN p.name, p.age ORDER BY p.age"))
    assert len(rows) == 2, f"expected 2 persons, got {len(rows)}"


def _q_2hop():
    rows = list(conn.execute(
        "MATCH (a:Person)-[:KNOWS]->(b:Person)-[:WORKS_FOR]->(c:Company) "
        "RETURN a.name, b.name, c.name"))
    assert len(rows) == 1, f"expected 1 two-hop row, got {len(rows)}"


check("create schema (2 node + 2 rel tables)", _schema)
check("insert persons/company/edges", _insert)
check("query persons == 2", _q_persons)
check("2-hop pattern query", _q_2hop)
conn.close()
db.close()

log("\n=== 2. extensions INSTALL / LOAD (from OSS) ===")
ext_db = Database("")
ext_conn = ext_db.connect()
matrix = {}
for ext in EXTENSIONS:
    ins = ld = False
    ins_err = ld_err = ""
    try:
        ext_conn.execute(f"INSTALL {ext}")
        ins = True
    except Exception as e:  # noqa: BLE001
        ins_err = f"{type(e).__name__}: {str(e).splitlines()[0][:160]}"
    if ins:
        try:
            ext_conn.execute(f"LOAD {ext}")
            ld = True
        except Exception as e:  # noqa: BLE001
            ld_err = f"{type(e).__name__}: {str(e).splitlines()[0][:160]}"
    matrix[ext] = (ins, ld)
    log(f"  [{'OK' if ins and ld else 'XX'}] {ext:<18} INSTALL={'Y' if ins else 'N'} LOAD={'Y' if ld else 'N'}")
    if ins_err:
        log(f"        install-error: {ins_err}")
    if ld_err:
        log(f"        load-error   : {ld_err}")

try:
    rows = list(ext_conn.execute("CALL SHOW_LOADED_EXTENSIONS() RETURN *;"))
    log(f"  SHOW_LOADED_EXTENSIONS -> {len(rows)} loaded: {[str(r[0]) for r in rows]}")
except Exception as e:  # noqa: BLE001
    log(f"  SHOW_LOADED_EXTENSIONS unavailable: {e}")
ext_conn.close()
ext_db.close()

ext_all_ok = all(ins and ld for ins, ld in matrix.values())
log("\n=== SUMMARY ===")
log(f"BASE_RESULT: {'PASS' if base_fail == 0 else 'FAIL'}")
log("EXT_MATRIX:")
for ext in EXTENSIONS:
    ins, ld = matrix[ext]
    log(f"  {ext:<18} INSTALL={'Y' if ins else 'N'} LOAD={'Y' if ld else 'N'}")
log(f"EXT_ALL_OK: {str(ext_all_ok).lower()}")
final_ok = base_fail == 0 and ext_all_ok
log(f"FINAL: {'PASS' if final_ok else 'FAIL'}")
sys.exit(0 if final_ok else 1)
