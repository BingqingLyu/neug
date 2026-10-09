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
"""

import json
import os
from collections import Counter

import pytest

from neug.database import Database

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
    # ODPS_SCAN in the message proves the binder routed the odps:// scheme to
    # this extension (T107's catalog-gated {SCHEME}_SCAN detection) rather than
    # mis-parsing the address as a file extension.
    assert "ODPS_SCAN" in message, message
    if _SDK_REQUIRED_MARKER not in message:
        # SDK build present: no SDK-required short-circuit fires. The live tests
        # own the data-path verification, so don't assert a marker that can't.
        pytest.skip("ODPS SDK build present; run with NEUG_ODPS_LIVE=1 for data.")


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
