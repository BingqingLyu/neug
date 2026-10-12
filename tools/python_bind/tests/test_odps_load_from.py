#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright 2020 Alibaba Group Holding Limited. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

"""End-to-end LOAD FROM tests for the odps extension.

Two layers, matching T107's acceptance ("`LOAD FROM "odps://proj/tbl" RETURN *`
columns/types/rows agree with ODPS"):

* Live integration tests (``test_odps_live_*``) -- the real acceptance. They run
  a ``LOAD FROM ... RETURN *`` and a ``COPY tbl FROM (LOAD FROM ...)`` against a
  genuine MaxCompute table and assert the rows actually came back / actually
  landed in the graph. They need the x86_64-only Storage API SDK build plus live
  credentials and VPC access, so they are gated behind ``NEUG_ODPS_LIVE=1`` and
  skip everywhere else (including this arm64 SDK-OFF dev host).

* Routing smoke tests (``test_odps_scheme_routes_*``) -- the host-runnable
  fallback. Without the SDK there is no data plane (sniff and the supplier both
  raise the SDK-required error), so these only prove the query routing wired up
  in T107: an ``odps://`` source resolves to this extension's ODPS_SCAN via the
  {SCHEME}_SCAN catalog lookup added to the binder, instead of the address being
  mis-parsed as a file extension (the trailing ``.my_table`` would otherwise be
  read as an extension and look up a non-existent ``MY_TABLE_SCAN``). They skip
  automatically when the SDK build is present, since the live tests then own the
  data-path verification.

Gated by NEUG_RUN_EXTENSION_TESTS like the other extension tests, because they
need the locally built libodps.neug_extension under the build root (published
wheels omit extensions).

Live-mode environment contract (only four variables, all read at test time):
  NEUG_ODPS_LIVE=1            master switch for the live tests
  ODPS_ACCESS_KEY_ID          real credentials, resolved by the C++ connection
  ODPS_ACCESS_KEY_SECRET      options builder (aliases also accepted)
  ODPS_ENDPOINT               service endpoint (VPC/intranet for the SDK build)
  NEUG_ODPS_TEST_PROJECT      the MaxCompute project that already holds the
                              fixed fixture tables documented below

Everything else -- table names, column lists, target DDL, expected rows, the
partition spec, and the pushdown predicates -- is hardcoded in this file (see
the "Fixed live fixtures" block after the imports). That keeps the live config
down to credentials + endpoint + project and lets the whole suite run in one
pytest session against a single known dataset.

Create these fixtures in NEUG_ODPS_TEST_PROJECT before running (BIGINT->INT64,
STRING->STRING, DOUBLE->DOUBLE under the ODPS->NeuG type mapping):

  -- Point/node table (5 rows).
  CREATE TABLE neug_odps_item (id BIGINT, name STRING, price DOUBLE);
  INSERT INTO neug_odps_item VALUES
    (1,'apple',9.5),(2,'banana',5.5),(3,'cherry',12.0),
    (4,'date',7.25),(5,'elderberry',3.75);

  -- Edge table: src/dst reference neug_odps_item.id. Only the (src,dst) pairs
  -- are asserted (against the edges read back out of the graph), so the `since`
  -- values are free.
  CREATE TABLE neug_odps_knows (src BIGINT, dst BIGINT, since STRING);
  INSERT INTO neug_odps_knows VALUES
    (1,2,'2020'),(2,3,'2021'),(3,4,'2022'),(4,5,'2023'),(1,5,'2024');

  -- Partitioned table (PARTITIONED BY pt STRING). pt=20260101 holds the first
  -- two item rows; pt=20260102 holds the other three. The partition-limited
  -- read asserts only pt=20260101 -> those two rows, which is what proves
  -- pruning reached ODPS instead of scanning every partition.
  CREATE TABLE neug_odps_item_pt (id BIGINT, name STRING, price DOUBLE)
    PARTITIONED BY (pt STRING);
  INSERT INTO neug_odps_item_pt PARTITION(pt='20260101') VALUES
    (1,'apple',9.5),(2,'banana',5.5);
  INSERT INTO neug_odps_item_pt PARTITION(pt='20260102') VALUES
    (3,'cherry',12.0),(4,'date',7.25),(5,'elderberry',3.75);

  -- All supported scalar types, one column each (T401/T405 type-coverage
  -- round-trip; the live analogue of example_dataset/comprehensive_graph's
  -- node_a.csv). Each column reads back through the documented ODPS->NeuG
  -- mapping: TINYINT/SMALLINT/INT->INT32, BIGINT->INT64, FLOAT->FLOAT,
  -- DOUBLE->DOUBLE, DECIMAL->DOUBLE (lossy), BOOLEAN->BOOL,
  -- STRING/VARCHAR/CHAR->STRING, DATE->DATE, DATETIME/TIMESTAMP->TIMESTAMP(ms).
  -- Adjust the typed-literal syntax below to your project's SQL type settings.
  CREATE TABLE neug_odps_all_types (
    c_tinyint TINYINT, c_smallint SMALLINT, c_int INT, c_bigint BIGINT,
    c_float FLOAT, c_double DOUBLE, c_decimal DECIMAL(10,2), c_boolean BOOLEAN,
    c_string STRING, c_varchar VARCHAR(20), c_char CHAR(5),
    c_date DATE, c_datetime DATETIME, c_timestamp TIMESTAMP);
  INSERT INTO neug_odps_all_types VALUES (
    -12, -1234, -123456789, 9223372036854775807,
    CAST(3.1415927 AS FLOAT), 2.718281828459045, CAST(123.45 AS DECIMAL(10,2)),
    true, 'test_string_0', 'vchar_a', 'chara',
    DATE '2023-01-15', DATETIME '2023-01-15 09:15:30',
    TIMESTAMP '2023-01-15 09:15:30.123');

  -- Unsupported-type fixture (T404/FR-018): a JSON column must be rejected at
  -- schema sniff with an actionable error (column name + MaxCompute type +
  -- STRING workaround), never silently decoded. BINARY / ARRAY / MAP / STRUCT
  -- behave the same; JSON is the one the spec calls out explicitly.
  CREATE TABLE neug_odps_unsupported_json (id BIGINT, payload JSON);
  INSERT INTO neug_odps_unsupported_json VALUES (1, JSON '{"k":"v"}');

The pushdown, remapping, and type-mismatch tests all reuse neug_odps_item (no
extra tables): pushdown filters `price > 5.0` (pushable) against the equivalent
`price + 0 > 5.0` (arithmetic -> engine-side), both yielding the 4 rows with
price > 5; remapping projects `id, price, name`; type-mismatch projects
`CAST(name, 'INT64')` against the STRING source, so the read *must* error
rather than silently emit the native value under the wrong type. The mismatch
is driven by the explicit CAST (the ODPS reader rejects a rewritten column type
at scan time, like the parquet scan's schema guard) rather than by the target
DDL alone, so it does not depend on the separate core COPY force-cast bug
alibaba/neug#1125 -- which would otherwise silently write garbage on a
COPY-from-(LOAD FROM) source/target type mismatch for every source (CSV too).
"""

import os
from collections import Counter
from datetime import datetime
from datetime import timedelta
from datetime import timezone

import pytest

from neug.database import Database
from neug.proto.error_pb2 import ERR_SCHEMA_MISMATCH
from neug.proto.error_pb2 import ERR_TYPE_CONVERSION

_TRUTHY = ("1", "true", "yes", "on")

_EXTENSION_TESTS_ENABLED = os.environ.get("NEUG_RUN_EXTENSION_TESTS", "").lower() in (
    _TRUTHY
)

pytestmark = pytest.mark.skipif(
    not _EXTENSION_TESTS_ENABLED,
    reason="Extension tests disabled by default; set NEUG_RUN_EXTENSION_TESTS=1.",
)

# Surfaces from the SDK-OFF build when a live sniff/read is attempted. Reaching
# it proves the query routed all the way into ODPS_SCAN.
_SDK_REQUIRED_MARKER = "requires the ODPS SDK build"

# Live integration mode: real MaxCompute table + SDK build + credentials/VPC.
# Off by default; the arm64 SDK-OFF dev host runs only the routing smoke tests.
_LIVE_ENABLED = os.environ.get("NEUG_ODPS_LIVE", "").lower() in _TRUTHY


def _load_odps(conn):
    """LOAD the locally built odps extension, skipping if it is not present."""
    try:
        conn.execute("LOAD odps;")
    except RuntimeError as error:
        pytest.skip(f"odps extension not available: {error}")


def _skip_if_sdk_off(error):
    """Skip a live test when the extension lacks the SDK data plane."""
    if _SDK_REQUIRED_MARKER in str(error):
        pytest.skip(f"odps extension built without the SDK: {error}")


def _freeze(row):
    """Make a result row hashable so rows can be compared as a multiset."""
    return tuple(tuple(value) if isinstance(value, list) else value for value in row)


def _assert_same_rows(actual, expected):
    """Compare two row collections ignoring order (ODPS has no stable sort)."""
    assert Counter(map(_freeze, actual)) == Counter(map(_freeze, expected))


# --- Fixed live fixtures ----------------------------------------------------
#
# Every live test reads from these hardcoded tables, which must pre-exist in the
# project named by NEUG_ODPS_TEST_PROJECT with exactly the schema and rows
# documented in the module docstring. Fixing them here (instead of threading a
# dozen env vars) keeps the live config down to credentials + endpoint + project
# and lets the whole suite run in one pytest session against a known dataset.

# Point/node table: neug_odps_item(id BIGINT, name STRING, price DOUBLE), 5 rows.
_ITEM_TABLE = "neug_odps_item"
_ITEM_COLUMNS = ["id", "name", "price"]
_ITEM_SCHEMA = "id INT64 PRIMARY KEY, name STRING, price DOUBLE"
_ITEM_ROWS = [
    [1, "apple", 9.5],
    [2, "banana", 5.5],
    [3, "cherry", 12.0],
    [4, "date", 7.25],
    [5, "elderberry", 3.75],
]
_NODE_KEY = "id"

# Edge table: neug_odps_knows(src BIGINT, dst BIGINT, since STRING); src/dst are
# neug_odps_item.id endpoints. Only the (src, dst) pairs are asserted (against
# the edges read back out of the graph), so the `since` values are unconstrained.
_EDGE_TABLE = "neug_odps_knows"
_EDGE_SRC = "src"
_EDGE_DST = "dst"
_EDGE_PROPS = ["since"]
_EDGE_PAIRS = [[1, 2], [2, 3], [3, 4], [4, 5], [1, 5]]

# Partitioned table: neug_odps_item_pt(id BIGINT, name STRING, price DOUBLE)
# PARTITIONED BY (pt STRING). The partition-limited read projects the three data
# columns explicitly, so the partition column never enters the compared rows
# regardless of how the reader surfaces it. pt=20260101 holds these two rows.
_PARTITION_TABLE = "neug_odps_item_pt"
_PARTITION_DATA_COLUMNS = ["id", "name", "price"]
_PARTITION_SPEC = "pt=20260101"
_PARTITION_ROWS = [
    [1, "apple", 9.5],
    [2, "banana", 5.5],
]

# Predicate-pushdown pair over neug_odps_item: `price > 5.0` is pushable (plain
# column vs literal); `price + 0 > 5.0` is semantically identical but has
# arithmetic, so the converter refuses it and NeuG filters engine-side. Both must
# yield exactly the four rows with price > 5 (elderberry 3.75 excluded).
_FILTER_PUSHABLE = "price > 5.0"
_FILTER_UNPUSHABLE = "price + 0 > 5.0"
_FILTER_ROWS = [
    [1, "apple", 9.5],
    [2, "banana", 5.5],
    [3, "cherry", 12.0],
    [4, "date", 7.25],
]

# Remapping projection over neug_odps_item: keep the key column first, reverse
# the rest -- proves COPY maps a reordered RETURN positionally without drift.
_REMAP_COLUMNS = ["id", "price", "name"]
_REMAP_PROJECTION = ", ".join(_REMAP_COLUMNS)
# The item rows reordered into the remapped column order (id, price, name).
_REMAP_ROWS = [[row[0], row[2], row[1]] for row in _ITEM_ROWS]

# Type-mismatch target DDL over neug_odps_item: `name` is STRING in the source
# but declared INT64 here, so COPY must fail rather than silently write.
_TYPE_MISMATCH_SCHEMA = "id INT64 PRIMARY KEY, name INT64, price DOUBLE"
# The mismatch is driven by an explicit CAST in the LOAD FROM projection rather
# than by the target DDL alone. NeuG core does not yet force-cast a COPY-from-
# (LOAD FROM) source column to a mismatched target type (alibaba/neug#1125), so
# relying on the DDL mismatch would silently write garbage. An explicit
# `CAST(name, 'INT64')` instead makes the read path itself reject the STRING
# source column (the ODPS reader returns native values without coercion and
# raises a schema-mismatch), which is the behaviour this test asserts and is
# not blocked by the core COPY bug.
_TYPE_MISMATCH_PROJECTION = "id, CAST(name, 'INT64') AS name, price"

# All-types coverage table: neug_odps_all_types, one column per supported
# MaxCompute scalar type (the live analogue of node_a.csv's type sweep). Each
# column reads back through the documented ODPS->NeuG mapping: TINYINT/
# SMALLINT/INT -> INT32, BIGINT -> INT64, FLOAT -> FLOAT, DOUBLE -> DOUBLE,
# DECIMAL -> DOUBLE (lossy), BOOLEAN -> BOOL, STRING/VARCHAR/CHAR -> STRING,
# DATE -> DATE, DATETIME/TIMESTAMP -> TIMESTAMP(ms). The single fixture row's
# expected values, in column order, are asserted by
# test_odps_live_all_types_round_trip.
_ALL_TYPES_TABLE = "neug_odps_all_types"
_ALL_TYPES_COLUMNS = [
    "c_tinyint",
    "c_smallint",
    "c_int",
    "c_bigint",
    "c_float",
    "c_double",
    "c_decimal",
    "c_boolean",
    "c_string",
    "c_varchar",
    "c_char",
    "c_date",
    "c_datetime",
    "c_timestamp",
]

# Unsupported-type table (T404/FR-018): neug_odps_unsupported_json(id BIGINT,
# payload JSON). Reading it must fail at schema sniff with an actionable error
# naming the column ("payload"), the MaxCompute type ("JSON"), and the STRING
# workaround -- the live counterpart of the host unit test
# UnsupportedTypeMessageIsActionable.
_UNSUPPORTED_JSON_TABLE = "neug_odps_unsupported_json"
_UNSUPPORTED_JSON_COLUMN = "payload"


def _live_project():
    """The ODPS project holding the fixed fixtures; skip when unset."""
    project = os.environ.get("NEUG_ODPS_TEST_PROJECT", "").strip()
    if not project:
        pytest.skip(
            "NEUG_ODPS_TEST_PROJECT not set; live tests need the ODPS project "
            "that holds the fixture tables."
        )
    return project


def _addr(table):
    """Build the odps:// address for a fixture table in the live project."""
    return f"odps://{_live_project()}.{table}"


def _live_credentials():
    """Skip unless real credentials/endpoint are present for live mode."""
    missing = [
        key
        for key in ("ODPS_ACCESS_KEY_ID", "ODPS_ACCESS_KEY_SECRET", "ODPS_ENDPOINT")
        if not os.environ.get(key, "").strip()
    ]
    if missing:
        pytest.skip(f"live ODPS credentials missing: {', '.join(missing)}")


# --- Live integration tests (SDK-ON + real credentials) ----------------------


@pytest.mark.skipif(
    not _LIVE_ENABLED,
    reason="live ODPS test; set NEUG_ODPS_LIVE=1 with real credentials to run.",
)
def test_odps_live_load_from_returns_table_data(tmp_path):
    """`LOAD FROM "odps://..." RETURN *` must return the real table rows.

    This is T107's Acceptance Scenario 1: the column names and row data coming
    back through the whole binder -> gopt -> execution -> ODPS_SCAN supplier
    chain must agree with what MaxCompute holds.
    """
    _live_credentials()
    table = _addr(_ITEM_TABLE)

    db = Database(db_path=str(tmp_path / "odps_live_load"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)

        try:
            result = conn.execute(f'LOAD FROM "{table}" RETURN *;')
        except RuntimeError as error:
            _skip_if_sdk_off(error)
            raise

        columns = result.column_names()
        rows = [list(row) for row in result]

        # Real data actually came back -- not an empty sniff-only result -- and
        # both the column names and every row match the neug_odps_item fixture.
        assert rows, f"live ODPS table {table!r} returned no rows"
        assert columns == _ITEM_COLUMNS, f"column mismatch: {columns}"
        _assert_same_rows(rows, _ITEM_ROWS)
    finally:
        conn.close()
        db.close()


@pytest.mark.skipif(
    not _LIVE_ENABLED,
    reason="live ODPS test; set NEUG_ODPS_LIVE=1 with real credentials to run.",
)
def test_odps_live_load_from_cast_to_incompatible_type_reports_error(tmp_path):
    """`LOAD FROM ... RETURN CAST(str_col, 'INT64')` must error, not pass through.

    The projection push-down optimizer folds a projection CAST into the scan
    column's declared type and strips the CAST, assuming the reader produces
    that type. The ODPS reader returns native Arrow values without coercion, so
    it rejects the rewritten column type at scan time (schema-mismatch) instead
    of silently returning the raw STRING under an INT64 label -- the eager
    counterpart of test_odps_live_copy_type_mismatch_reports_error.
    """
    _live_credentials()
    table = _addr(_ITEM_TABLE)

    db = Database(db_path=str(tmp_path / "odps_live_cast"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        with pytest.raises(RuntimeError) as excinfo:
            conn.execute(f'LOAD FROM "{table}" RETURN {_TYPE_MISMATCH_PROJECTION};')
        message = str(excinfo.value)
        _skip_if_sdk_off(excinfo.value)
        assert (
            str(ERR_TYPE_CONVERSION) in message or str(ERR_SCHEMA_MISMATCH) in message
        ), message
    finally:
        conn.close()
        db.close()


@pytest.mark.skipif(
    not _LIVE_ENABLED,
    reason="live ODPS test; set NEUG_ODPS_LIVE=1 with real credentials to run.",
)
def test_odps_live_copy_from_load_imports_rows(tmp_path):
    """`COPY tbl FROM (LOAD FROM "odps://...")` must really import the rows.

    Reading the rows back out of the graph with MATCH is what proves the data
    was materialized into storage (the supplier/COPY-fusion path), not merely
    parsed or sniffed.
    """
    _live_credentials()
    table = _addr(_ITEM_TABLE)
    projection = ", ".join(_ITEM_COLUMNS)
    readback_projection = ", ".join(f"n.{column}" for column in _ITEM_COLUMNS)

    db = Database(db_path=str(tmp_path / "odps_live_copy"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        conn.execute(f"CREATE NODE TABLE item({_ITEM_SCHEMA});")

        try:
            conn.execute(f'COPY item FROM (LOAD FROM "{table}" RETURN {projection});')
        except RuntimeError as error:
            _skip_if_sdk_off(error)
            raise

        # Read back from the graph: this is the "data really imported" check,
        # held against the neug_odps_item ground-truth rows.
        imported = [
            list(row)
            for row in conn.execute(f"MATCH (n:item) RETURN {readback_projection};")
        ]
        assert imported, "COPY FROM (LOAD FROM) imported no rows into the graph"
        _assert_same_rows(imported, _ITEM_ROWS)
    finally:
        conn.close()
        db.close()


@pytest.mark.skipif(
    not _LIVE_ENABLED,
    reason="live ODPS test; set NEUG_ODPS_LIVE=1 with real credentials to run.",
)
def test_odps_live_partition_limited_read(tmp_path):
    """`LOAD FROM "odps://...?<partition>" RETURN id, name, price` reads one pt.

    T109 / Acceptance Scenario 2 (spec M1 Integration Tests: partition-limited
    read): a partition-qualified address must return exactly the rows of that
    single partition. The projection names the three data columns explicitly, so
    the partition column never enters the comparison. Comparing against the
    known per-partition ground truth (neug_odps_item_pt, pt=20260101) is what
    proves the partition filter reached ODPS (pruning) instead of scanning every
    partition.
    """
    _live_credentials()
    address = f"{_addr(_PARTITION_TABLE)}?{_PARTITION_SPEC}"
    projection = ", ".join(_PARTITION_DATA_COLUMNS)

    db = Database(db_path=str(tmp_path / "odps_live_partition"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        try:
            result = conn.execute(f'LOAD FROM "{address}" RETURN {projection};')
        except RuntimeError as error:
            _skip_if_sdk_off(error)
            raise
        rows = [list(row) for row in result]
        assert rows, f"partition {_PARTITION_SPEC!r} of {address!r} returned no rows"
        _assert_same_rows(rows, _PARTITION_ROWS)
    finally:
        conn.close()
        db.close()


@pytest.mark.skipif(
    not _LIVE_ENABLED,
    reason="live ODPS test; set NEUG_ODPS_LIVE=1 with real credentials to run.",
)
def test_odps_live_missing_table_reports_not_found(tmp_path):
    """A non-existent table must surface a locatable not-found error.

    T109 / Acceptance Scenario 3 + FR-005 (spec M1 Integration Tests: the
    table-not-found error path): the attributed error must point at
    "table/project does not exist or no access" (OdpsError NotFound ->
    NotFoundException in C++) and, per SC-006, must never echo the real
    AccessKey secret.
    """
    _live_credentials()
    secret = os.environ.get("ODPS_ACCESS_KEY_SECRET", "")
    missing = _addr(f"{_ITEM_TABLE}_neug_t109_missing")

    db = Database(db_path=str(tmp_path / "odps_live_missing"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        with pytest.raises(RuntimeError) as excinfo:
            conn.execute(f'LOAD FROM "{missing}" RETURN *;')
        message = str(excinfo.value)
        _skip_if_sdk_off(excinfo.value)
        lowered = message.lower()
        assert any(
            token in lowered
            for token in (
                "not exist",
                "does not exist",
                "not found",
                "no access",
                "nosuch",
                "no permission",
            )
        ), message
        if secret:
            assert secret not in message, "AccessKey secret leaked into the error"
    finally:
        conn.close()
        db.close()


@pytest.mark.skipif(
    not _LIVE_ENABLED,
    reason="live ODPS test; set NEUG_ODPS_LIVE=1 with real credentials to run.",
)
def test_odps_live_bad_credentials_reports_auth_failure_without_plaintext(
    tmp_path, monkeypatch
):
    """Bad credentials must surface an auth failure with no AccessKey plaintext.

    T109 / Acceptance Scenario 4 + FR-005/SC-006 (spec M1 Integration Tests:
    the auth-failure error path): with a real endpoint but deliberately wrong
    keys, the attributed error must indicate an authentication/authorization
    failure (OdpsError Authentication -> PermissionDeniedException in C++) and
    must not contain the supplied secret in clear text.
    """
    table = _addr(_ITEM_TABLE)
    endpoint = os.environ.get("ODPS_ENDPOINT", "").strip()
    if not endpoint:
        pytest.skip("ODPS_ENDPOINT required to reach a real auth failure.")

    bogus_secret = "NeugT109BogusSecretKey0000000000"
    monkeypatch.setenv("ODPS_ACCESS_KEY_ID", "LTAI5tBogusKeyId000000")
    monkeypatch.setenv("ODPS_ACCESS_KEY_SECRET", bogus_secret)
    # Make sure the bogus keys are the ones resolved: drop the ALIBABA_CLOUD
    # aliases so they cannot shadow ODPS_ACCESS_KEY_ID/SECRET.
    monkeypatch.delenv("ALIBABA_CLOUD_ACCESS_KEY_ID", raising=False)
    monkeypatch.delenv("ALIBABA_CLOUD_ACCESS_KEY_SECRET", raising=False)

    db = Database(db_path=str(tmp_path / "odps_live_badcreds"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        with pytest.raises(RuntimeError) as excinfo:
            conn.execute(f'LOAD FROM "{table}" RETURN *;')
        message = str(excinfo.value)
        _skip_if_sdk_off(excinfo.value)
        lowered = message.lower()
        assert any(
            token in lowered
            for token in (
                "authenticat",
                "authoriz",
                "unauthorized",
                "accessdenied",
                "access denied",
                "signature",
                "forbidden",
                "invalidaccesskey",
                "permission",
                "credential",
            )
        ), message
        assert bogus_secret not in message, "AccessKey secret leaked into the error"
    finally:
        conn.close()
        db.close()


@pytest.mark.skipif(
    not _LIVE_ENABLED,
    reason="live ODPS test; set NEUG_ODPS_LIVE=1 with real credentials to run.",
)
def test_odps_live_copy_edge_from_load_imports_edges(tmp_path):
    """`COPY <edge> FROM (LOAD FROM "odps://...") (from=, to=)` imports edges.

    T202 / Acceptance Scenario 2 (spec M2): the first two RETURN columns are
    bound to the source/destination primary keys and resolved against the
    already-imported node table; any remaining columns become edge properties.
    The (src, dst) pairs read back with MATCH must equal the neug_odps_knows
    fixture pairs, proving every edge imported AND that the two key columns
    mapped to the correct endpoints (no swap / misalignment).
    """
    _live_credentials()
    node_table = _addr(_ITEM_TABLE)
    edge_table = _addr(_EDGE_TABLE)
    node_projection = ", ".join(_ITEM_COLUMNS)
    edge_projection = ", ".join([_EDGE_SRC, _EDGE_DST] + _EDGE_PROPS)

    db = Database(db_path=str(tmp_path / "odps_live_edge"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        conn.execute(f"CREATE NODE TABLE person({_ITEM_SCHEMA});")
        try:
            # Endpoints first: the edge COPY resolves src/dst against person's
            # primary key, so the node table must already hold those keys.
            conn.execute(
                f'COPY person FROM (LOAD FROM "{node_table}" '
                f"RETURN {node_projection});"
            )
            conn.execute(
                f'COPY knows FROM (LOAD FROM "{edge_table}" '
                f'RETURN {edge_projection}) (from="person", to="person");'
            )
        except RuntimeError as error:
            _skip_if_sdk_off(error)
            raise

        imported = [
            [row[0], row[1]]
            for row in conn.execute(
                "MATCH (a:person)-[k:knows]->(b:person) "
                f"RETURN a.{_NODE_KEY}, b.{_NODE_KEY};"
            )
        ]
        assert imported, f"live ODPS edge table {edge_table!r} imported no edges"
        _assert_same_rows(imported, _EDGE_PAIRS)
    finally:
        conn.close()
        db.close()


@pytest.mark.skipif(
    not _LIVE_ENABLED,
    reason="live ODPS test; set NEUG_ODPS_LIVE=1 with real credentials to run.",
)
def test_odps_live_copy_from_load_remapping_matches_projection(tmp_path):
    """A reordered/subset RETURN must map into the target without misalignment.

    T203 / Acceptance Scenario 3 + FR-009 (spec M2): the RETURN projection
    reorders and subsets the ODPS columns (applied by odps `execFunc`'s
    project_chunk on the fallback COPY path); the auto-created node table takes
    them positionally. Reading the graph back in that projected order must equal
    the item rows reordered the same way, proving no column drifted.
    """
    _live_credentials()
    table = _addr(_ITEM_TABLE)
    projection = _REMAP_PROJECTION  # "id, price, name"
    readback = ", ".join(f"n.{name}" for name in _REMAP_COLUMNS)

    db = Database(db_path=str(tmp_path / "odps_live_remap"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        try:
            conn.execute(
                f'COPY remap_node FROM (LOAD FROM "{table}" RETURN {projection});'
            )
        except RuntimeError as error:
            _skip_if_sdk_off(error)
            raise

        # The auto-created remap_node table stores the projected columns
        # positionally (id, price, name); reading them back in that order must
        # equal the item rows reordered the same way -- no column drifted.
        imported = [
            list(row)
            for row in conn.execute(f"MATCH (n:remap_node) RETURN {readback};")
        ]
        assert imported, "remapping COPY imported no rows into the graph"
        _assert_same_rows(imported, _REMAP_ROWS)
    finally:
        conn.close()
        db.close()


@pytest.mark.skipif(
    not _LIVE_ENABLED,
    reason="live ODPS test; set NEUG_ODPS_LIVE=1 with real credentials to run.",
)
def test_odps_live_copy_type_mismatch_reports_error(tmp_path):
    """An incompatible source column type must error, never silently write.

    T204 / FR-010 (spec M2): when a LOAD FROM projection asks for a type the
    ODPS source column cannot provide, the read must fail with a type-
    conversion or schema-mismatch error (locating the problem) rather than
    emitting the native value under the wrong type. The projection casts the
    STRING `name` column of neug_odps_item to INT64.

    The mismatch is expressed as an explicit `CAST(name, 'INT64')` in the LOAD
    FROM projection (not via the target DDL) so the assertion does not depend
    on the separate core COPY force-cast bug alibaba/neug#1125: the ODPS reader
    rejects the rewritten column type at scan time.
    """
    _live_credentials()
    table = _addr(_ITEM_TABLE)
    projection = _TYPE_MISMATCH_PROJECTION

    db = Database(db_path=str(tmp_path / "odps_live_typemismatch"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        conn.execute(f"CREATE NODE TABLE bad_typed({_TYPE_MISMATCH_SCHEMA});")
        with pytest.raises(RuntimeError) as excinfo:
            conn.execute(
                f'COPY bad_typed FROM (LOAD FROM "{table}" RETURN {projection});'
            )
        message = str(excinfo.value)
        _skip_if_sdk_off(excinfo.value)
        assert (
            str(ERR_TYPE_CONVERSION) in message or str(ERR_SCHEMA_MISMATCH) in message
        ), message
    finally:
        conn.close()
        db.close()


@pytest.mark.skipif(
    not _LIVE_ENABLED,
    reason="live ODPS test; set NEUG_ODPS_LIVE=1 with real credentials to run.",
)
def test_odps_live_predicate_pushdown_matches_engine_filter(tmp_path):
    """A pushed predicate and its engine-side equivalent return identical rows.

    T304 / FR-013 + FR-015 (spec M3: pushdown fallback and result equivalence).
    The pushable clause is translated to an ODPS predicate and filtered
    server-side (the transfer-reduction path); the equivalent non-pushable
    clause is NOT translated, so ODPS returns every row and NeuG's
    reader::filter_chunk does the filtering engine-side (the fallback path).
    Both must yield the exact same row multiset -- and the ground truth when
    supplied -- which is what proves pushdown never changes the result and the
    fallback stays correct. Mirrors parquet's
    test_reader_preserves_complete_predicates, where pushable and non-pushable
    predicates alike return the expected rows.
    """
    _live_credentials()
    table = _addr(_ITEM_TABLE)
    pushable = _FILTER_PUSHABLE
    unpushable = _FILTER_UNPUSHABLE
    projection = ", ".join(_ITEM_COLUMNS)

    db = Database(db_path=str(tmp_path / "odps_live_pushdown"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        try:
            full = [
                list(row)
                for row in conn.execute(f'LOAD FROM "{table}" RETURN {projection};')
            ]
            pushed = [
                list(row)
                for row in conn.execute(
                    f'LOAD FROM "{table}" WHERE {pushable} RETURN {projection};'
                )
            ]
            engine = [
                list(row)
                for row in conn.execute(
                    f'LOAD FROM "{table}" WHERE {unpushable} RETURN {projection};'
                )
            ]
        except RuntimeError as error:
            _skip_if_sdk_off(error)
            raise

        # Server-side pushdown == engine-side fallback, row for row (FR-013/015).
        _assert_same_rows(pushed, engine)
        # A filter can only remove rows, never add them: the pushed read is a
        # subset of the unfiltered scan (SC-004 rows-read reduction observable).
        assert len(pushed) <= len(full), "filter returned more rows than the scan"
        # And both equal the fixture ground truth (the 4 rows with price > 5).
        _assert_same_rows(pushed, _FILTER_ROWS)
    finally:
        conn.close()
        db.close()


@pytest.mark.skipif(
    not _LIVE_ENABLED,
    reason="live ODPS test; set NEUG_ODPS_LIVE=1 with real credentials to run.",
)
def test_odps_live_all_types_round_trip(tmp_path):
    """Every supported MaxCompute scalar type maps and decodes end-to-end.

    T401/T405 (spec M4 Acceptance 1, SC-002): reads the neug_odps_all_types
    fixture -- one column per supported MaxCompute scalar type, mirroring
    node_a.csv's type sweep -- and asserts each comes back as the documented
    NeuG type/value. This is the live counterpart of the host unit tests
    (MapsDecimalToDouble / MapsTemporalTypes / ConvertsDecimal128ToDouble): it
    proves the real Storage API Arrow encodings (decimal128 `d:p,s`, date32,
    timestamp) flow through the bridge, which only a genuine MaxCompute read can
    exercise. DECIMAL is read as a DOUBLE (lossy) and DATETIME/TIMESTAMP as
    millisecond TIMESTAMPs, exactly as documented in
    doc/source/extensions/odps.md.
    """
    _live_credentials()
    table = _addr(_ALL_TYPES_TABLE)

    db = Database(db_path=str(tmp_path / "odps_live_all_types"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        try:
            result = conn.execute(f'LOAD FROM "{table}" RETURN *;')
        except RuntimeError as error:
            _skip_if_sdk_off(error)
            raise

        columns = result.column_names()
        rows = [list(row) for row in result]
        assert rows, f"live ODPS table {table!r} returned no rows"
        assert columns == _ALL_TYPES_COLUMNS, f"column mismatch: {columns}"

        row = rows[0]
        # Integer widths narrower than 64 bits widen to INT32; BIGINT -> INT64.
        assert row[0] == -12  # c_tinyint  -> INT32
        assert row[1] == -1234  # c_smallint -> INT32
        assert row[2] == -123456789  # c_int      -> INT32
        assert row[3] == 9223372036854775807  # c_bigint   -> INT64 (max)
        assert abs(row[4] - 3.1415927) < 1e-6  # c_float    -> FLOAT
        assert abs(row[5] - 2.718281828459045) < 1e-9  # c_double -> DOUBLE
        assert abs(row[6] - 123.45) < 1e-9  # c_decimal -> DOUBLE (lossy)
        assert bool(row[7]) is True  # c_boolean -> BOOL
        assert str(row[8]) == "test_string_0"  # c_string  -> STRING
        assert str(row[9]) == "vchar_a"  # c_varchar -> STRING
        assert str(row[10]) == "chara"  # c_char    -> STRING
        assert str(row[11]) == "2023-01-15"  # c_date -> DATE (no tz shift)
        # DATETIME/TIMESTAMP -> millisecond TIMESTAMP. MaxCompute stores them as
        # instants in the project timezone (odps.sql.timezone; the fixture
        # project graphscope_dev is Asia/Shanghai, UTC+8) and the Storage API
        # transmits the epoch instant, which the reader renders as a UTC
        # wall-clock (it drops the Arrow tz annotation; NeuG's TIMESTAMP is
        # timezone-naive). So the inserted 09:15:30 (+08:00) reads back as
        # 01:15:30 UTC -- the SAME instant, not a corrupted value -- with the
        # .123 ms fraction preserved on c_timestamp. See the extension docs,
        # "Precision and timezone".
        project_tz = timezone(timedelta(hours=8))
        inserted = datetime(2023, 1, 15, 9, 15, 30, tzinfo=project_tz)
        expected_utc = inserted.astimezone(timezone.utc).replace(tzinfo=None)
        assert row[12] == expected_utc  # c_datetime -> UTC instant (to second)
        # c_timestamp keeps millisecond precision (.123 -> 123000 microseconds).
        assert row[13] == expected_utc.replace(microsecond=123000)
    finally:
        conn.close()
        db.close()


@pytest.mark.skipif(
    not _LIVE_ENABLED,
    reason="live ODPS test; set NEUG_ODPS_LIVE=1 with real credentials to run.",
)
def test_odps_live_unsupported_column_type_reports_error(tmp_path):
    """An unsupported column type (JSON) must fail with an actionable error.

    T404 / FR-018 (spec M4): reading a table with a JSON column must raise an
    error that names the column, the MaxCompute type, and the STRING workaround,
    rather than silently producing wrong data. The rejection happens at schema
    sniff (off the ODPS column metadata), so it is deterministic regardless of
    how the value would be Arrow-encoded. Live counterpart of the host unit test
    UnsupportedTypeMessageIsActionable.
    """
    _live_credentials()
    table = _addr(_UNSUPPORTED_JSON_TABLE)

    db = Database(db_path=str(tmp_path / "odps_live_unsupported"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        with pytest.raises(RuntimeError) as excinfo:
            conn.execute(f'LOAD FROM "{table}" RETURN *;')
        message = str(excinfo.value)
        _skip_if_sdk_off(excinfo.value)
        lowered = message.lower()
        assert _UNSUPPORTED_JSON_COLUMN in lowered, message
        assert "json" in lowered, message
        assert "string" in lowered, message  # the documented workaround
    finally:
        conn.close()
        db.close()


# --- Routing smoke tests (SDK-OFF host fallback) -----------------------------


def _set_dummy_credentials(monkeypatch):
    """Provide throwaway credentials/endpoint so connection-option resolution
    (which runs before sniffFunc) succeeds and the scan reaches the SDK gate.

    The SDK-OFF sniff short-circuits to its error before any network call, so
    these values are never used to contact a real service. monkeypatch restores
    the environment afterwards, keeping the test hermetic and order-independent.
    """
    monkeypatch.setenv("ODPS_ACCESS_KEY_ID", "routing-dummy-id")
    monkeypatch.setenv("ODPS_ACCESS_KEY_SECRET", "routing-dummy-secret")
    monkeypatch.setenv("ODPS_ENDPOINT", "routing-dummy-endpoint")


def _assert_routed_to_odps_scan(message):
    # Reaching the ODPS data path proves the binder routed the odps:// scheme to
    # this extension's ODPS_SCAN operator (T107's catalog-gated {SCHEME}_SCAN
    # detection) rather than mis-parsing the address as a file extension.
    #
    # The two builds surface this differently:
    #   * SDK-OFF skeleton: sniffFunc short-circuits with _SDK_REQUIRED_MARKER
    #     and the operator name ODPS_SCAN is embedded in the error. Assert both
    #     so a routing regression (which produces neither) still fails here.
    #   * SDK-ON: the scan reaches the real connection layer, which fails on the
    #     dummy credentials with an odps_connection/[LocalError] error that does
    #     NOT name ODPS_SCAN. Routing is nonetheless proven; the live tests own
    #     the data-path verification, so skip rather than assert a marker that
    #     cannot fire in this build.
    if _SDK_REQUIRED_MARKER in message:
        assert "ODPS_SCAN" in message, message
        return
    if "odps_connection" in message or "ODPS_SCAN" in message:
        pytest.skip("ODPS SDK build present; run with NEUG_ODPS_LIVE=1 for data.")
    # Neither the SDK-required marker nor an ODPS connection error: routing
    # genuinely failed (e.g. a catalog error naming a mis-parsed fragment).
    assert "ODPS_SCAN" in message, message


@pytest.mark.skipif(
    _LIVE_ENABLED,
    reason="NEUG_ODPS_LIVE=1: the live tests cover routing end-to-end.",
)
def test_odps_scheme_routes_to_odps_scan(tmp_path, monkeypatch):
    """`LOAD FROM "odps://project.schema.table" RETURN *` must reach ODPS_SCAN.

    A regression in binder scheme detection would instead fail earlier with a
    catalog error naming a mis-parsed fragment of the address (e.g. a
    "MY_TABLE_SCAN does not exist" style error), never mentioning ODPS_SCAN.
    """
    _set_dummy_credentials(monkeypatch)

    db = Database(db_path=str(tmp_path / "odps_routing"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)

        with pytest.raises(RuntimeError) as excinfo:
            conn.execute('LOAD FROM "odps://my_project.default.my_table" RETURN *;')
        _assert_routed_to_odps_scan(str(excinfo.value))
    finally:
        conn.close()
        db.close()


@pytest.mark.skipif(
    _LIVE_ENABLED,
    reason="NEUG_ODPS_LIVE=1: the live tests cover routing end-to-end.",
)
def test_odps_scheme_routes_through_copy_from_load(tmp_path, monkeypatch):
    """`COPY tbl FROM (LOAD FROM "odps://...")` must route to ODPS_SCAN too.

    This is the supplier/COPY-fusion entry point of T107. The LOAD FROM
    subquery is bound (and sniffed) before column mapping, so the SDK-required
    error surfaces here exactly as it does for the plain RETURN form.
    """
    _set_dummy_credentials(monkeypatch)

    db = Database(db_path=str(tmp_path / "odps_copy_routing"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        conn.execute("CREATE NODE TABLE item(id INT64 PRIMARY KEY, name STRING);")

        with pytest.raises(RuntimeError) as excinfo:
            conn.execute(
                'COPY item FROM (LOAD FROM "odps://my_project.default.my_table" '
                "RETURN id, name);"
            )
        _assert_routed_to_odps_scan(str(excinfo.value))
    finally:
        conn.close()
        db.close()


@pytest.mark.skipif(
    _LIVE_ENABLED,
    reason="NEUG_ODPS_LIVE=1: the live tests cover routing end-to-end.",
)
def test_odps_scheme_routes_through_copy_edge_from_load(tmp_path, monkeypatch):
    """`COPY <edge> FROM (LOAD FROM "odps://...") (from=, to=)` reaches ODPS_SCAN.

    T202 host-runnable half: the edge COPY-fusion entry point resolves the
    from/to node tables, then binds (and sniffs) the LOAD FROM subquery before
    any data lands, so the SDK-required error surfaces here exactly as for the
    node COPY form. That proves the edge path routes to this extension's
    ODPS_SCAN instead of mis-parsing the odps:// address as a file extension.
    """
    _set_dummy_credentials(monkeypatch)

    db = Database(db_path=str(tmp_path / "odps_copy_edge_routing"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        conn.execute("CREATE NODE TABLE person(id INT64 PRIMARY KEY, name STRING);")

        with pytest.raises(RuntimeError) as excinfo:
            conn.execute(
                'COPY knows FROM (LOAD FROM "odps://my_project.default.my_edge" '
                'RETURN src, dst, weight) (from="person", to="person");'
            )
        _assert_routed_to_odps_scan(str(excinfo.value))
    finally:
        conn.close()
        db.close()
