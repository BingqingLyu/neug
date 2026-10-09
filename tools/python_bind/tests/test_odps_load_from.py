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

Live-mode environment contract (all read at test time):
  NEUG_ODPS_LIVE=1            master switch for the live tests
  ODPS_ACCESS_KEY_ID          real credentials, resolved by the C++ connection
  ODPS_ACCESS_KEY_SECRET      options builder (aliases also accepted)
  ODPS_ENDPOINT               service endpoint (VPC/intranet for the SDK build)
  NEUG_ODPS_TEST_TABLE        required: odps://[project.][schema.]table address
  NEUG_ODPS_TEST_COLUMNS      optional: the full ordered column list of the
                              table, comma-separated (validated against
                              RETURN * and reused as the COPY projection)
  NEUG_ODPS_TEST_SCHEMA       optional: node-table DDL body for the COPY test
                              (e.g. "id INT64 PRIMARY KEY, name STRING")
  NEUG_ODPS_TEST_ROWS         optional: JSON list of expected rows
                              (e.g. [[1,"alpha"],[2,"beta"]])
  NEUG_ODPS_TEST_PARTITION    optional: partition spec for the partition-limited
                              read (e.g. "pt=20260921"), appended to the table
                              address as "?<spec>"
  NEUG_ODPS_TEST_PARTITION_ROWS
                              optional: JSON list of the rows expected in that
                              single partition (the ground truth that proves
                              partition pruning happened)

Module 2 (COPY <table> FROM (LOAD FROM ...)) live-mode additions:
  NEUG_ODPS_NODE_KEY          the node table's PRIMARY KEY column; edge COPY
                              resolves its src/dst endpoints against it, so it
                              must be the key the edge table references
  NEUG_ODPS_EDGE_TABLE        odps:// address of the edge source table
  NEUG_ODPS_EDGE_SRC          column of NEUG_ODPS_EDGE_TABLE holding the source
                              node key (bound to the edge's FROM endpoint)
  NEUG_ODPS_EDGE_DST          column of NEUG_ODPS_EDGE_TABLE holding the
                              destination node key (bound to the TO endpoint)
  NEUG_ODPS_EDGE_PROPS        optional: comma-separated edge property columns,
                              appended after src, dst in the RETURN projection
  NEUG_ODPS_REMAP_PROJECTION  optional: an explicit reordered/subset RETURN body
                              for the remapping test; when unset it is derived
                              from NEUG_ODPS_TEST_COLUMNS (first column kept as
                              the auto-created table's key, the rest reversed)
  NEUG_ODPS_TYPE_MISMATCH_SCHEMA
                              required by the type-check test: a node-table DDL
                              body whose column type deliberately cannot accept
                              the source column (e.g. INT64 declared for a
                              STRING column), so COPY must error rather than
                              silently write wrong data (FR-010)
  NEUG_ODPS_TYPE_MISMATCH_PROJECTION
                              optional: RETURN body paired with the mismatched
                              DDL (defaults to NEUG_ODPS_TEST_COLUMNS)

Module 3 (predicate-pushdown equivalence, T304) live-mode additions:
  NEUG_ODPS_TEST_FILTER       required by the pushdown-equivalence test: a WHERE
                              clause the converter CAN push down (whitelisted
                              comparisons / IN / IS NULL / AND / OR / NOT over
                              plain columns and literals), e.g. "age > 30"
  NEUG_ODPS_TEST_FILTER_UNPUSHABLE
                              required by the same test: a WHERE clause that is
                              semantically EQUIVALENT to NEUG_ODPS_TEST_FILTER
                              but that the converter will NOT push (arithmetic,
                              CAST, CASE, a scalar function, or a dynamic
                              param), e.g. "age + 0 > 30". Forcing the
                              engine-side fallback is what makes the two reads a
                              fair equivalence check.
  NEUG_ODPS_TEST_FILTER_ROWS  optional: JSON list of the rows the filter should
                              yield (the ground truth both reads are held to)
"""

import json
import os
from collections import Counter

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


# --- Live configuration helpers ---------------------------------------------


def _live_table():
    """The odps:// address under test; required for every live test."""
    table = os.environ.get("NEUG_ODPS_TEST_TABLE", "").strip()
    if not table:
        pytest.skip("NEUG_ODPS_TEST_TABLE not set; live test needs a real table.")
    return table


def _live_credentials():
    """Skip unless real credentials/endpoint are present for live mode."""
    missing = [
        key
        for key in ("ODPS_ACCESS_KEY_ID", "ODPS_ACCESS_KEY_SECRET", "ODPS_ENDPOINT")
        if not os.environ.get(key, "").strip()
    ]
    if missing:
        pytest.skip(f"live ODPS credentials missing: {', '.join(missing)}")


def _live_columns():
    """Optional projected column list, or None when not configured."""
    raw = os.environ.get("NEUG_ODPS_TEST_COLUMNS", "").strip()
    if not raw:
        return None
    return [column.strip() for column in raw.split(",") if column.strip()]


def _live_expected_rows():
    """Optional expected rows decoded from NEUG_ODPS_TEST_ROWS JSON."""
    raw = os.environ.get("NEUG_ODPS_TEST_ROWS", "").strip()
    if not raw:
        return None
    return json.loads(raw)


def _live_partition():
    """Partition spec for the partition-limited read; skip when not set."""
    partition = os.environ.get("NEUG_ODPS_TEST_PARTITION", "").strip()
    if not partition:
        pytest.skip("NEUG_ODPS_TEST_PARTITION not set; no partition to limit to.")
    return partition.lstrip("?")


def _live_partition_rows():
    """Ground-truth rows of the single partition under test."""
    raw = os.environ.get("NEUG_ODPS_TEST_PARTITION_ROWS", "").strip()
    if not raw:
        pytest.skip(
            "NEUG_ODPS_TEST_PARTITION_ROWS not set; cannot prove partition pruning."
        )
    return json.loads(raw)


def _live_node_key():
    """Node PRIMARY KEY column that edge endpoints resolve against."""
    key = os.environ.get("NEUG_ODPS_NODE_KEY", "").strip()
    if not key:
        pytest.skip("NEUG_ODPS_NODE_KEY not set; cannot bind edge endpoints.")
    return key


def _live_edge_source():
    """Edge table + src/dst key columns; skip unless all three are present."""
    table = os.environ.get("NEUG_ODPS_EDGE_TABLE", "").strip()
    src = os.environ.get("NEUG_ODPS_EDGE_SRC", "").strip()
    dst = os.environ.get("NEUG_ODPS_EDGE_DST", "").strip()
    missing = [
        name
        for name, value in (
            ("NEUG_ODPS_EDGE_TABLE", table),
            ("NEUG_ODPS_EDGE_SRC", src),
            ("NEUG_ODPS_EDGE_DST", dst),
        )
        if not value
    ]
    if missing:
        pytest.skip(f"live edge source incomplete: {', '.join(missing)}")
    return table, src, dst


def _live_edge_props():
    """Optional edge property columns appended after src, dst."""
    raw = os.environ.get("NEUG_ODPS_EDGE_PROPS", "").strip()
    if not raw:
        return []
    return [column.strip() for column in raw.split(",") if column.strip()]


def _live_remap_projection(columns):
    """A reordered/subset RETURN body that exercises column remapping.

    Defaults to keeping the first column (so the auto-created node table gets a
    valid primary key) and reversing the rest -- enough to prove the projection
    is applied identically through COPY and a direct read. Set
    NEUG_ODPS_REMAP_PROJECTION for a bespoke reorder/subset/alias scenario.
    """
    override = os.environ.get("NEUG_ODPS_REMAP_PROJECTION", "").strip()
    if override:
        return override
    if len(columns) < 3:
        pytest.skip(
            "NEUG_ODPS_TEST_COLUMNS needs >= 3 columns to derive a remapping "
            "projection (or set NEUG_ODPS_REMAP_PROJECTION)."
        )
    return ", ".join([columns[0]] + list(reversed(columns[1:])))


def _live_type_mismatch_schema():
    """A node DDL whose column type deliberately clashes with the source."""
    schema = os.environ.get("NEUG_ODPS_TYPE_MISMATCH_SCHEMA", "").strip()
    if not schema:
        pytest.skip(
            "NEUG_ODPS_TYPE_MISMATCH_SCHEMA not set; no incompatible target "
            "type to check against."
        )
    return schema


def _live_filter():
    """A pushable WHERE clause for the pushdown-equivalence test."""
    clause = os.environ.get("NEUG_ODPS_TEST_FILTER", "").strip()
    if not clause:
        pytest.skip("NEUG_ODPS_TEST_FILTER not set; no pushable predicate.")
    return clause


def _live_filter_unpushable():
    """An equivalent-but-non-pushable WHERE clause (forces engine filtering)."""
    clause = os.environ.get("NEUG_ODPS_TEST_FILTER_UNPUSHABLE", "").strip()
    if not clause:
        pytest.skip(
            "NEUG_ODPS_TEST_FILTER_UNPUSHABLE not set; no engine-side equivalent "
            "to compare the pushed read against."
        )
    return clause


def _live_filter_rows():
    """Optional ground-truth rows the filter should yield, or None."""
    raw = os.environ.get("NEUG_ODPS_TEST_FILTER_ROWS", "").strip()
    if not raw:
        return None
    return json.loads(raw)


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
    table = _live_table()

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

        # Real data actually came back -- not an empty sniff-only result.
        assert rows, f"live ODPS table {table!r} returned no rows"
        assert columns, "LOAD FROM RETURN * produced no column names"

        expected_columns = _live_columns()
        if expected_columns is not None:
            assert columns == expected_columns, f"column mismatch: {columns}"

        expected_rows = _live_expected_rows()
        if expected_rows is not None:
            _assert_same_rows(rows, expected_rows)
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
    table = _live_table()

    columns = _live_columns()
    if columns is None:
        pytest.skip("NEUG_ODPS_TEST_COLUMNS required for the COPY import test.")
    schema = os.environ.get("NEUG_ODPS_TEST_SCHEMA", "").strip()
    if not schema:
        pytest.skip("NEUG_ODPS_TEST_SCHEMA required for the COPY import test.")

    projection = ", ".join(columns)
    readback_projection = ", ".join(f"n.{column}" for column in columns)

    db = Database(db_path=str(tmp_path / "odps_live_copy"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        conn.execute(f"CREATE NODE TABLE item({schema});")

        try:
            conn.execute(f'COPY item FROM (LOAD FROM "{table}" RETURN {projection});')
        except RuntimeError as error:
            _skip_if_sdk_off(error)
            raise

        # Read back from the graph: this is the "data really imported" check.
        imported = [
            list(row)
            for row in conn.execute(f"MATCH (n:item) RETURN {readback_projection};")
        ]
        assert imported, "COPY FROM (LOAD FROM) imported no rows into the graph"

        expected_rows = _live_expected_rows()
        if expected_rows is not None:
            _assert_same_rows(imported, expected_rows)
        else:
            # No fixed expectation: the graph must agree with a direct read of
            # the same table, so the import is self-consistent (rows + count).
            direct = [
                list(row)
                for row in conn.execute(f'LOAD FROM "{table}" RETURN {projection};')
            ]
            _assert_same_rows(imported, direct)
    finally:
        conn.close()
        db.close()


@pytest.mark.skipif(
    not _LIVE_ENABLED,
    reason="live ODPS test; set NEUG_ODPS_LIVE=1 with real credentials to run.",
)
def test_odps_live_partition_limited_read(tmp_path):
    """`LOAD FROM "odps://...?<partition>" RETURN *` reads only that partition.

    T109 / Acceptance Scenario 2 (spec M1 Integration Tests: partition-limited
    read): a partition-qualified address must return exactly the rows of that
    single partition. Comparing against the known per-partition ground truth is
    what proves the partition filter reached ODPS (pruning) instead of scanning
    the whole table.
    """
    _live_credentials()
    table = _live_table()
    partition = _live_partition()
    expected_rows = _live_partition_rows()
    address = f"{table}?{partition}"

    db = Database(db_path=str(tmp_path / "odps_live_partition"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        try:
            result = conn.execute(f'LOAD FROM "{address}" RETURN *;')
        except RuntimeError as error:
            _skip_if_sdk_off(error)
            raise
        rows = [list(row) for row in result]
        assert rows, f"partition {partition!r} of {table!r} returned no rows"
        _assert_same_rows(rows, expected_rows)
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
    table = _live_table()
    secret = os.environ.get("ODPS_ACCESS_KEY_SECRET", "")
    missing = f"{table}_neug_t109_missing"

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
    table = _live_table()
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
    The (src, dst) pairs read back with MATCH must equal a direct read of the
    same ODPS edge table, proving every edge imported AND that the two key
    columns mapped to the correct endpoints (no swap / misalignment).
    """
    _live_credentials()
    node_table = _live_table()
    node_columns = _live_columns()
    node_schema = os.environ.get("NEUG_ODPS_TEST_SCHEMA", "").strip()
    if node_columns is None or not node_schema:
        pytest.skip(
            "NEUG_ODPS_TEST_COLUMNS and NEUG_ODPS_TEST_SCHEMA are required to "
            "import the edge endpoints' node table."
        )
    node_key = _live_node_key()
    edge_table, edge_src, edge_dst = _live_edge_source()
    edge_props = _live_edge_props()

    node_projection = ", ".join(node_columns)
    edge_projection = ", ".join([edge_src, edge_dst] + edge_props)

    db = Database(db_path=str(tmp_path / "odps_live_edge"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        conn.execute(f"CREATE NODE TABLE person({node_schema});")
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

        direct = [
            [row[0], row[1]]
            for row in conn.execute(
                f'LOAD FROM "{edge_table}" RETURN {edge_src}, {edge_dst};'
            )
        ]
        assert direct, f"live ODPS edge table {edge_table!r} returned no rows"
        imported = [
            [row[0], row[1]]
            for row in conn.execute(
                "MATCH (a:person)-[k:knows]->(b:person) "
                f"RETURN a.{node_key}, b.{node_key};"
            )
        ]
        _assert_same_rows(imported, direct)
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
    them positionally. Reading the graph back in the same projected order must
    equal a direct LOAD FROM with that projection, proving no column drifted.
    """
    _live_credentials()
    table = _live_table()
    columns = _live_columns()
    if columns is None:
        pytest.skip("NEUG_ODPS_TEST_COLUMNS required for the remapping test.")
    projection = _live_remap_projection(columns)

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

        # Projected names come from the direct read, so aliases in a bespoke
        # NEUG_ODPS_REMAP_PROJECTION are handled too; the auto-created
        # remap_node table stores exactly these columns, positionally.
        direct_result = conn.execute(f'LOAD FROM "{table}" RETURN {projection};')
        projected_names = direct_result.column_names()
        direct = [list(row) for row in direct_result]
        readback = ", ".join(f"n.{name}" for name in projected_names)
        imported = [
            list(row)
            for row in conn.execute(f"MATCH (n:remap_node) RETURN {readback};")
        ]
        assert imported, "remapping COPY imported no rows into the graph"
        _assert_same_rows(imported, direct)
    finally:
        conn.close()
        db.close()


@pytest.mark.skipif(
    not _LIVE_ENABLED,
    reason="live ODPS test; set NEUG_ODPS_LIVE=1 with real credentials to run.",
)
def test_odps_live_copy_type_mismatch_reports_error(tmp_path):
    """An incompatible target column type must error, never silently write.

    T204 / FR-010 (spec M2): when the declared node-table column type cannot
    accept the ODPS source column, COPY must fail with a type-conversion or
    schema-mismatch error (locating the problem) rather than writing wrong
    data. The mismatched DDL is supplied by the live config, which knows the
    real source column types.
    """
    _live_credentials()
    table = _live_table()
    schema = _live_type_mismatch_schema()
    projection = os.environ.get("NEUG_ODPS_TYPE_MISMATCH_PROJECTION", "").strip()
    if not projection:
        columns = _live_columns()
        if columns is None:
            pytest.skip(
                "NEUG_ODPS_TEST_COLUMNS or NEUG_ODPS_TYPE_MISMATCH_PROJECTION "
                "required to pair with the mismatched DDL."
            )
        projection = ", ".join(columns)

    db = Database(db_path=str(tmp_path / "odps_live_typemismatch"), mode="w")
    conn = db.connect()
    try:
        _load_odps(conn)
        conn.execute(f"CREATE NODE TABLE bad_typed({schema});")
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
    table = _live_table()
    pushable = _live_filter()
    unpushable = _live_filter_unpushable()
    columns = _live_columns()
    projection = ", ".join(columns) if columns else "*"

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

        ground_truth = _live_filter_rows()
        if ground_truth is not None:
            _assert_same_rows(pushed, ground_truth)
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
