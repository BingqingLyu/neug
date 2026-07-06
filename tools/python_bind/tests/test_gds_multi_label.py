#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright 2020 Alibaba Group Holding Limited. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Tests for multi-label graph projection in GDS algorithms.
# Uses tinysnb dataset which has person, organisation, knows, workAt.

import os
import sys
from contextlib import contextmanager

import pytest

sys.path.append(os.path.join(os.path.dirname(__file__), "../"))

from neug.database import Database

# Skip if GDS extension tests not enabled.
pytestmark = pytest.mark.skipif(
    os.environ.get("NEUG_RUN_EXTENSION_TESTS") != "1",
    reason="GDS extension tests disabled (set NEUG_RUN_EXTENSION_TESTS=1)",
)


@contextmanager
def multi_label_connection(tmp_path):
    """Open a writable DB with tinysnb loaded (has person, organisation, knows, workAt)."""
    db_dir = tmp_path / "gds_multi_label_db"
    db = Database(db_path=str(db_dir), mode="w")
    db.load_builtin_dataset("tinysnb")
    conn = db.connect()
    try:
        conn.execute("LOAD gds;")
        yield conn
    finally:
        conn.close()
        db.close()


# =============================================================================
# Multi-label algorithm tests
# =============================================================================


def test_wcc_multi_label(tmp_path):
    """WCC on multi-label projection: person + organisation via knows and workAt."""
    with multi_label_connection(tmp_path) as conn:
        conn.execute(
            "CALL project_graph("
            "'multi_g', "
            "['person', 'organisation'], "
            "{'[person, knows, person]': '', '[person, workAt, organisation]': ''}"
            ");"
        )
        rows = list(
            conn.execute(
                """
                CALL wcc('multi_g', {concurrency: 2})
                YIELD node, comp
                RETURN node, comp;
                """
            )
        )
        assert len(rows) > 0, "WCC must return at least one row"
        for row in rows:
            assert row[0] is not None, "node should not be None"
            assert isinstance(row[1], int), "comp should be an integer"


def test_page_rank_multi_label(tmp_path):
    """PageRank on multi-label projection."""
    with multi_label_connection(tmp_path) as conn:
        conn.execute(
            "CALL project_graph("
            "'multi_g', "
            "['person', 'organisation'], "
            "{'[person, knows, person]': '', '[person, workAt, organisation]': ''}"
            ");"
        )
        rows = list(
            conn.execute(
                """
                CALL page_rank('multi_g', {max_iterations: 10, concurrency: 2})
                YIELD node, rank
                RETURN node, rank;
                """
            )
        )
        assert len(rows) > 0, "PageRank must return at least one row"
        ranks = [row[1] for row in rows]
        assert all(r >= 0 for r in ranks), "All ranks must be non-negative"


def test_leiden_multi_label(tmp_path):
    """Leiden community detection on multi-label projection."""
    with multi_label_connection(tmp_path) as conn:
        conn.execute(
            "CALL project_graph("
            "'multi_g', "
            "['person', 'organisation'], "
            "{'[person, knows, person]': '', '[person, workAt, organisation]': ''}"
            ");"
        )
        rows = list(
            conn.execute(
                """
                CALL leiden('multi_g', {concurrency: 2})
                YIELD node, community
                RETURN node, community;
                """
            )
        )
        assert len(rows) > 0, "Leiden must return at least one row"
        for row in rows:
            assert isinstance(row[1], int), "community should be an integer"


# =============================================================================
# Single-label backward compatibility
# =============================================================================


def test_wcc_single_label_still_works(tmp_path):
    """Single-label WCC still works as before (backward compat)."""
    with multi_label_connection(tmp_path) as conn:
        conn.execute(
            "CALL project_graph("
            "'single_g', "
            "['person'], "
            "{'[person, knows, person]': ''}"
            ");"
        )
        rows = list(
            conn.execute(
                """
                CALL wcc('single_g', {concurrency: 2})
                YIELD node, comp
                RETURN node, comp;
                """
            )
        )
        assert len(rows) > 0, "single-label WCC must return results"


# =============================================================================
# Validation tests (T303)
# =============================================================================


def test_reject_heterogeneous_subgraph(tmp_path):
    """Reject subgraph where edge endpoints are not in vertex label set."""
    with multi_label_connection(tmp_path) as conn:
        with pytest.raises(Exception) as exc_info:
            conn.execute(
                "CALL project_graph("
                "'bad_g', "
                "['person'], "
                "{'[person, workAt, organisation]': ''}"
                ");"
            )
        assert "not projected" in str(exc_info.value)


def test_reject_predicate_in_multi_label(tmp_path):
    """Reject predicates in multi-label projections."""
    with multi_label_connection(tmp_path) as conn:
        conn.execute(
            "CALL project_graph("
            "'pred_g', "
            "{'person': 'n.age > 20', 'organisation': ''}, "
            "{'[person, knows, person]': '', '[person, workAt, organisation]': ''}"
            ");"
        )
        with pytest.raises(Exception) as exc_info:
            conn.execute(
                """
                CALL wcc('pred_g', {concurrency: 1})
                YIELD node, comp
                RETURN node, comp;
                """
            )
        assert "Invalid subgraph" in str(exc_info.value) or \
               "Predicates are not supported in multi-label" in str(exc_info.value)
