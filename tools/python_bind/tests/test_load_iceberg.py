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

"""Tests for LOAD FROM with the Iceberg extension.

These tests verify that the Iceberg extension can:
  - Load and register the ICEBERG_SCAN function
  - Read Iceberg table data via LOAD FROM
  - Support column projection (RETURN specific columns)
  - Support row filtering (WHERE clause)
  - Infer schema from Iceberg metadata
"""

import os
import shutil
import subprocess
import sys

import pytest

sys.path.append(os.path.join(os.path.dirname(__file__), "../"))

from neug import Database

EXTENSION_TESTS_ENABLED = os.environ.get("NEUG_RUN_EXTENSION_TESTS", "").lower() in (
    "1",
    "true",
    "yes",
    "on",
)
extension_test = pytest.mark.skipif(
    not EXTENSION_TESTS_ENABLED,
    reason="Extension tests disabled; set NEUG_RUN_EXTENSION_TESTS=1 to enable.",
)


def _get_generator_script():
    """Return absolute path to the Iceberg test data generator script."""
    current_file = os.path.abspath(__file__)
    tests_dir = os.path.dirname(current_file)
    python_bind_dir = os.path.dirname(tests_dir)
    tools_dir = os.path.dirname(python_bind_dir)
    workspace_root = os.path.dirname(tools_dir)
    return os.path.join(
        workspace_root, "example_dataset", "iceberg", "generate_test_data.py"
    )


def _generate_iceberg_data(target_dir: str):
    """Generate Iceberg test data in target_dir using a subprocess.

    We use a subprocess to avoid PyArrow dynamic-linking conflicts
    with the statically-linked Arrow inside the neug module.
    """
    script = _get_generator_script()
    if not os.path.exists(script):
        pytest.skip(f"Iceberg generator script not found: {script}")
    result = subprocess.run(
        [sys.executable, script, "simple", target_dir],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        pytest.fail(
            f"Iceberg data generation failed:\n"
            f"stdout: {result.stdout}\n"
            f"stderr: {result.stderr}"
        )


@extension_test
class TestLoadFromIceberg:
    """Test LOAD FROM with the Iceberg extension."""

    @pytest.fixture(autouse=True)
    def setup(self, tmp_path):
        """Set up database and generate Iceberg test data."""
        # Generate Iceberg test data in a temp directory
        self.iceberg_path = str(tmp_path / "iceberg_table")
        _generate_iceberg_data(self.iceberg_path)

        # Create NeuG database
        self.db_dir = str(tmp_path / "test_iceberg_db")
        shutil.rmtree(self.db_dir, ignore_errors=True)
        self.db = Database(db_path=self.db_dir, mode="w")
        self.conn = self.db.connect()

        # Load iceberg extension
        self.conn.execute("load iceberg")

        yield

        self.conn.close()
        self.db.close()

    def test_load_iceberg_return_all(self):
        """Test basic LOAD FROM Iceberg table with RETURN *."""
        query = f"""
        LOAD FROM "{self.iceberg_path}" (FILE_FORMAT='iceberg')
        RETURN *
        """
        result = self.conn.execute(query)
        records = list(result)

        # Data file contains 5 rows
        assert len(records) == 5, f"Expected 5 records, got {len(records)}"

        # Should have 3 columns: id, name, value
        first_record = records[0]
        assert len(first_record) == 3, f"Expected 3 columns, got {len(first_record)}"

    def test_load_iceberg_return_specific_columns(self):
        """Test LOAD FROM Iceberg with column projection."""
        query = f"""
        LOAD FROM "{self.iceberg_path}" (FILE_FORMAT='iceberg')
        RETURN id, name
        """
        result = self.conn.execute(query)
        records = list(result)

        assert len(records) == 5, f"Expected 5 records, got {len(records)}"

        # Should return exactly 2 columns
        first_record = records[0]
        assert len(first_record) == 2, f"Expected 2 columns, got {len(first_record)}"

    def test_load_iceberg_data_values(self):
        """Test that LOAD FROM Iceberg returns correct data values."""
        query = f"""
        LOAD FROM "{self.iceberg_path}" (FILE_FORMAT='iceberg')
        RETURN id, name, value
        """
        result = self.conn.execute(query)
        records = list(result)

        assert len(records) == 5, f"Expected 5 records, got {len(records)}"

        # Verify data content (order may vary, so collect into a set)
        names = {str(r[1]) for r in records}
        expected_names = {"Alice", "Bob", "Charlie", "Diana", "Eve"}
        assert names == expected_names, f"Expected names {expected_names}, got {names}"

    def test_load_iceberg_with_where(self):
        """Test LOAD FROM Iceberg with WHERE clause filtering."""
        query = f"""
        LOAD FROM "{self.iceberg_path}" (FILE_FORMAT='iceberg')
        WHERE value > 30.0
        RETURN id, name, value
        """
        result = self.conn.execute(query)
        records = list(result)

        # value > 30.0 should return: Charlie(30.7), Diana(40.1), Eve(50.9)
        assert len(records) == 3, f"Expected 3 records with value > 30.0, got {len(records)}"

        for record in records:
            assert float(record[2]) > 30.0, f"Expected value > 30.0, got {record[2]}"

    def test_load_iceberg_schema_inference(self):
        """Test that Iceberg schema is correctly inferred."""
        query = f"""
        LOAD FROM "{self.iceberg_path}" (FILE_FORMAT='iceberg')
        RETURN id, name, value
        """
        result = self.conn.execute(query)
        records = list(result)

        assert len(records) > 0, "Should have at least one record"

        # Check column types from first record
        first = records[0]
        assert isinstance(first[0], int), f"id should be int, got {type(first[0])}"
        assert isinstance(first[1], str), f"name should be str, got {type(first[1])}"
        assert isinstance(first[2], float), f"value should be float, got {type(first[2])}"


def _get_delete_generator_script():
    """Return absolute path to the Iceberg delete test data generator script."""
    current_file = os.path.abspath(__file__)
    tests_dir = os.path.dirname(current_file)
    python_bind_dir = os.path.dirname(tests_dir)
    tools_dir = os.path.dirname(python_bind_dir)
    workspace_root = os.path.dirname(tools_dir)
    return os.path.join(
        workspace_root, "example_dataset", "iceberg", "generate_test_data_with_deletes.py"
    )


def _generate_iceberg_data_with_deletes(target_dir: str):
    """Generate Iceberg test data with delete files."""
    script = _get_delete_generator_script()
    if not os.path.exists(script):
        pytest.skip(f"Iceberg delete generator script not found: {script}")
    result = subprocess.run(
        [sys.executable, script, target_dir],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        pytest.fail(
            f"Iceberg delete data generation failed:\n"
            f"stdout: {result.stdout}\n"
            f"stderr: {result.stderr}"
        )


@extension_test
class TestLoadFromIcebergWithDeletes:
    """Test LOAD FROM with Iceberg delete files (FR-008)."""

    @pytest.fixture(autouse=True)
    def setup(self, tmp_path):
        """Set up database and generate Iceberg test data with deletes."""
        self.iceberg_path = str(tmp_path / "iceberg_table_deletes")
        _generate_iceberg_data_with_deletes(self.iceberg_path)

        self.db_dir = str(tmp_path / "test_iceberg_delete_db")
        shutil.rmtree(self.db_dir, ignore_errors=True)
        self.db = Database(db_path=self.db_dir, mode="w")
        self.conn = self.db.connect()
        self.conn.execute("load iceberg")

        yield

        self.conn.close()
        self.db.close()

    def test_delete_files_filter_rows(self):
        """Test that positional and equality deletes remove correct rows.

        Data: id=[1,2,3,4,5], name=[Alice,Bob,Charlie,David,Eve]
        Positional delete: row 1 (Bob)
        Equality delete: id=4 (David)
        Expected result: id=[1,3,5], name=[Alice,Charlie,Eve]
        """
        query = f"""
        LOAD FROM "{self.iceberg_path}" (FILE_FORMAT='iceberg')
        RETURN id, name, value
        """
        result = self.conn.execute(query)
        records = list(result)

        assert len(records) == 3, f"Expected 3 records after deletes, got {len(records)}"

        # Collect IDs and names
        ids = sorted([int(r[0]) for r in records])
        names = sorted([str(r[1]) for r in records])

        assert ids == [1, 3, 5], f"Expected ids [1, 3, 5], got {ids}"
        assert names == sorted(["Alice", "Charlie", "Eve"]), (
            f"Expected names [Alice, Charlie, Eve], got {names}"
        )

    def test_delete_files_with_column_projection(self):
        """Test that deletes work correctly with column projection."""
        query = f"""
        LOAD FROM "{self.iceberg_path}" (FILE_FORMAT='iceberg')
        RETURN id, name
        """
        result = self.conn.execute(query)
        records = list(result)

        assert len(records) == 3, f"Expected 3 records after deletes, got {len(records)}"
        # Each record should have exactly 2 columns
        assert all(len(r) == 2 for r in records), "Expected 2 columns per record"


# ─── Partitioned Iceberg table tests (predicate pushdown / pruning) ──────

def _generate_partitioned_iceberg_data(target_dir: str):
    """Generate partitioned Iceberg test data using generate_test_data.py."""
    script = _get_generator_script()
    if not os.path.exists(script):
        pytest.skip(f"Iceberg generator script not found: {script}")
    result = subprocess.run(
        [sys.executable, script, "partitioned", target_dir],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        pytest.fail(
            f"Partitioned Iceberg data generation failed:\n"
            f"stdout: {result.stdout}\n"
            f"stderr: {result.stderr}"
        )


@extension_test
class TestLoadFromIcebergPruning:
    """Test predicate pushdown with manifest/data-file pruning.

    Test data layout (9 rows, 3 data files, 2 manifests):
      manifest-A  (category=A):
        file-A1: id=[1,2,3], value=[10.5, 20.3, 30.7]
        file-A2: id=[4,5,6], value=[40.1, 50.9, 60.2]
      manifest-B  (category=B):
        file-B1: id=[7,8,9], value=[70.4, 80.6, 90.8]
    """

    @pytest.fixture(autouse=True)
    def setup(self, tmp_path):
        self.iceberg_path = str(tmp_path / "iceberg_partitioned")
        _generate_partitioned_iceberg_data(self.iceberg_path)

        self.db_dir = str(tmp_path / "test_iceberg_prune_db")
        shutil.rmtree(self.db_dir, ignore_errors=True)
        self.db = Database(db_path=self.db_dir, mode="w")
        self.conn = self.db.connect()
        self.conn.execute("load iceberg")

        yield

        self.conn.close()
        self.db.close()

    def test_return_all(self):
        """All 9 rows from all 3 files."""
        query = f"""
        LOAD FROM "{self.iceberg_path}" (FILE_FORMAT='iceberg')
        RETURN *
        """
        records = list(self.conn.execute(query))
        assert len(records) == 9, f"Expected 9 records, got {len(records)}"

    def test_filter_category_eq_a(self):
        """Level-1 prune: manifest-B skipped entirely."""
        query = f"""
        LOAD FROM "{self.iceberg_path}" (FILE_FORMAT='iceberg')
        WHERE category = 'A'
        RETURN id, name, value, category
        """
        records = list(self.conn.execute(query))
        assert len(records) == 6, f"Expected 6 records for category=A, got {len(records)}"
        for r in records:
            assert str(r[3]) == "A", f"Expected category A, got {r[3]}"

    def test_filter_value_gt_65(self):
        """Level-2 prune: files A1, A2 skipped (upper <= 65)."""
        query = f"""
        LOAD FROM "{self.iceberg_path}" (FILE_FORMAT='iceberg')
        WHERE value > 65.0
        RETURN id, value
        """
        records = list(self.conn.execute(query))
        assert len(records) == 3, f"Expected 3 records with value>65, got {len(records)}"
        for r in records:
            assert float(r[1]) > 65.0, f"Expected value>65, got {r[1]}"

    def test_filter_category_a_and_value_gt_35(self):
        """Level-1 prune B, Level-2 prune A1 → only file-A2 read."""
        query = f"""
        LOAD FROM "{self.iceberg_path}" (FILE_FORMAT='iceberg')
        WHERE category = 'A' AND value > 35.0
        RETURN id, name, value, category
        """
        records = list(self.conn.execute(query))
        # file-A2: value [40.1, 50.9, 60.2]  (all > 35)
        assert len(records) == 3, f"Expected 3 records, got {len(records)}"
        for r in records:
            assert str(r[3]) == "A"
            assert float(r[2]) > 35.0

    def test_filter_category_eq_c_returns_empty(self):
        """Level-1 prune all manifests → empty result."""
        query = f"""
        LOAD FROM "{self.iceberg_path}" (FILE_FORMAT='iceberg')
        WHERE category = 'C'
        RETURN id
        """
        records = list(self.conn.execute(query))
        assert len(records) == 0, f"Expected 0 records for category=C, got {len(records)}"

    def test_filter_value_lt_25(self):
        """Level-2 prune A2, B1 → only file-A1 read."""
        query = f"""
        LOAD FROM "{self.iceberg_path}" (FILE_FORMAT='iceberg')
        WHERE value < 25.0
        RETURN id, value
        """
        records = list(self.conn.execute(query))
        # file-A1 values: 10.5, 20.3, 30.7 → filter value<25 → 10.5, 20.3
        assert len(records) == 2, f"Expected 2 records with value<25, got {len(records)}"
        for r in records:
            assert float(r[1]) < 25.0

    def test_parquet_row_filter_no_pruning(self):
        """Level-3 (Parquet row-group pushdown) — no manifest or file pruned.

        WHERE value > 20.0:
          Level-1: no pruning (value is not a partition column)
          Level-2: no pruning (all files have upper_bound > 20.0)
          Level-3: Parquet scanner filters out value <= 20.0 within files
            A1 [10.5, 20.3, 30.7] → keep 20.3, 30.7 (2 rows)
            A2 [40.1, 50.9, 60.2] → keep all      (3 rows)
            B1 [70.4, 80.6, 90.8] → keep all      (3 rows)
          Total: 8 rows
        """
        query = f"""
        LOAD FROM "{self.iceberg_path}" (FILE_FORMAT='iceberg')
        WHERE value > 20.0
        RETURN id, value, category
        """
        records = list(self.conn.execute(query))
        assert len(records) == 8, f"Expected 8 records with value>20, got {len(records)}"
        for r in records:
            assert float(r[1]) > 20.0, f"Expected value>20.0, got {r[1]}"

