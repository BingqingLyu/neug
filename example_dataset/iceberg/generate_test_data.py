#!/usr/bin/env python3
"""Generate a minimal Iceberg table structure for testing.

This script creates a self-contained Iceberg table directory with:
  - data/00000-0-data.parquet        (5 rows: id, name, value)
  - manifests/manifest-1.parquet     (manifest file pointing to data file)
  - manifests/snap-manifest-list.parquet  (manifest list pointing to manifest)
  - metadata/v1.metadata.json        (table metadata with schema + snapshot)

Usage:
    python3 generate_test_data.py [output_dir]
    Default output_dir: same directory as this script.
"""

import json
import os
import sys

import pyarrow as pa
import pyarrow.parquet as pq


def generate(output_dir: str):
    os.makedirs(os.path.join(output_dir, "data"), exist_ok=True)
    os.makedirs(os.path.join(output_dir, "manifests"), exist_ok=True)
    os.makedirs(os.path.join(output_dir, "metadata"), exist_ok=True)

    # ── 1. Data file ────────────────────────────────────────────────────
    data_schema = pa.schema(
        [
            pa.field("id", pa.int64()),
            pa.field("name", pa.string()),
            pa.field("value", pa.float64()),
        ]
    )
    data_table = pa.table(
        {
            "id": [1, 2, 3, 4, 5],
            "name": ["Alice", "Bob", "Charlie", "Diana", "Eve"],
            "value": [10.5, 20.3, 30.7, 40.1, 50.9],
        },
        schema=data_schema,
    )
    data_path = os.path.join(output_dir, "data", "00000-0-data.parquet")
    pq.write_table(data_table, data_path)
    data_file_size = os.path.getsize(data_path)

    # ── 2. Manifest file (Parquet) ──────────────────────────────────────
    # Uses flattened column names that our parser supports.
    manifest_schema = pa.schema(
        [
            pa.field("status", pa.int32()),
            pa.field("file_path", pa.string()),
            pa.field("file_format", pa.string()),
            pa.field("record_count", pa.int64()),
            pa.field("file_size_in_bytes", pa.int64()),
        ]
    )
    manifest_table = pa.table(
        {
            "status": [1],  # 1 = added
            "file_path": ["data/00000-0-data.parquet"],
            "file_format": ["PARQUET"],
            "record_count": [5],
            "file_size_in_bytes": [data_file_size],
        },
        schema=manifest_schema,
    )
    manifest_path = os.path.join(output_dir, "manifests", "manifest-1.parquet")
    pq.write_table(manifest_table, manifest_path)
    manifest_file_size = os.path.getsize(manifest_path)

    # ── 3. Manifest list (Parquet) ──────────────────────────────────────
    manifest_list_schema = pa.schema(
        [
            pa.field("manifest_path", pa.string()),
            pa.field("manifest_length", pa.int64()),
            pa.field("partition_spec_id", pa.int32()),
            pa.field("content", pa.int32()),
            pa.field("added_snapshot_id", pa.int64()),
            pa.field("added_data_files_count", pa.int32()),
            pa.field("existing_data_files_count", pa.int32()),
            pa.field("deleted_data_files_count", pa.int32()),
        ]
    )
    manifest_list_table = pa.table(
        {
            "manifest_path": ["manifests/manifest-1.parquet"],
            "manifest_length": [manifest_file_size],
            "partition_spec_id": [0],
            "content": [0],  # 0 = data
            "added_snapshot_id": [1000000000],
            "added_data_files_count": [1],
            "existing_data_files_count": [0],
            "deleted_data_files_count": [0],
        },
        schema=manifest_list_schema,
    )
    manifest_list_path = os.path.join(
        output_dir, "manifests", "snap-manifest-list.parquet"
    )
    pq.write_table(manifest_list_table, manifest_list_path)

    # ── 4. Metadata JSON ───────────────────────────────────────────────
    metadata = {
        "format-version": 1,
        "table-uuid": "test-iceberg-table-uuid-0001",
        "location": output_dir,
        "schema": {
            "type": "struct",
            "fields": [
                {"id": 1, "name": "id", "required": False, "type": "long"},
                {"id": 2, "name": "name", "required": False, "type": "string"},
                {"id": 3, "name": "value", "required": False, "type": "double"},
            ],
        },
        "partition-spec": [],
        "default-spec-id": 0,
        "last-partition-id": 0,
        "properties": {},
        "current-snapshot-id": 1000000000,
        "snapshots": [
            {
                "snapshot-id": 1000000000,
                "timestamp-ms": 1700000000000,
                "manifest-list": "manifests/snap-manifest-list.parquet",
                "summary": {
                    "operation": "append",
                    "added-data-files": "1",
                    "total-records": "5",
                },
            }
        ],
        "snapshot-log": [
            {"timestamp-ms": 1700000000000, "snapshot-id": 1000000000}
        ],
    }
    metadata_path = os.path.join(output_dir, "metadata", "v1.metadata.json")
    with open(metadata_path, "w") as f:
        json.dump(metadata, f, indent=2)

    print(f"Iceberg test data generated at: {output_dir}")
    print(f"  Data file:      {data_path} ({data_file_size} bytes)")
    print(f"  Manifest file:  {manifest_path} ({manifest_file_size} bytes)")
    print(f"  Manifest list:  {manifest_list_path}")
    print(f"  Metadata:       {metadata_path}")


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    generate(out)
