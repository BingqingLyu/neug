#!/usr/bin/env python3
"""Generate Iceberg table test data WITH delete files.

Creates an Iceberg table structure that includes:
  - data/00000-0-data.parquet         (5 rows: id, name, value)
  - deletes/pos-delete.parquet        (positional delete: row 1)
  - deletes/eq-delete.parquet         (equality delete: id=4)
  - manifests/data-manifest.parquet   (manifest for data file)
  - manifests/pos-del-manifest.parquet (manifest for positional delete)
  - manifests/eq-del-manifest.parquet  (manifest for equality delete)
  - manifests/snap-manifest-list.parquet (manifest list)
  - metadata/v1.metadata.json         (table metadata with format-version=2)

Expected result after filtering: rows with id=1,3,5
  - id=2 (row index 1) is removed by positional delete
  - id=4 is removed by equality delete
"""

import json
import os
import sys

import pyarrow as pa
import pyarrow.parquet as pq


def generate(output_dir: str):
    os.makedirs(os.path.join(output_dir, "data"), exist_ok=True)
    os.makedirs(os.path.join(output_dir, "deletes"), exist_ok=True)
    os.makedirs(os.path.join(output_dir, "manifests"), exist_ok=True)
    os.makedirs(os.path.join(output_dir, "metadata"), exist_ok=True)

    # ── 1. Data file ─────────────────────────────────────────────────
    data_schema = pa.schema([
        pa.field("id", pa.int64()),
        pa.field("name", pa.string()),
        pa.field("value", pa.float64()),
    ])
    data_table = pa.table(
        {
            "id": [1, 2, 3, 4, 5],
            "name": ["Alice", "Bob", "Charlie", "David", "Eve"],
            "value": [10.5, 20.3, 30.7, 40.1, 50.9],
        },
        schema=data_schema,
    )
    data_path = os.path.join(output_dir, "data", "00000-0-data.parquet")
    pq.write_table(data_table, data_path)
    data_file_size = os.path.getsize(data_path)

    # ── 2. Positional delete file ─────────────────────────────────────
    # Delete row at position 1 (Bob) from the data file
    pos_del_schema = pa.schema([
        pa.field("file_path", pa.string()),
        pa.field("pos", pa.int64()),
    ])
    pos_del_table = pa.table(
        {
            "file_path": [data_path],
            "pos": [1],  # 0-based: row 1 = "Bob"
        },
        schema=pos_del_schema,
    )
    pos_del_path = os.path.join(output_dir, "deletes", "pos-delete.parquet")
    pq.write_table(pos_del_table, pos_del_path)
    pos_del_size = os.path.getsize(pos_del_path)

    # ── 3. Equality delete file ────────────────────────────────────────
    # Delete rows where id=4 (David)
    eq_del_schema = pa.schema([
        pa.field("id", pa.int64()),
    ])
    eq_del_table = pa.table(
        {
            "id": [4],
        },
        schema=eq_del_schema,
    )
    eq_del_path = os.path.join(output_dir, "deletes", "eq-delete.parquet")
    pq.write_table(eq_del_table, eq_del_path)
    eq_del_size = os.path.getsize(eq_del_path)

    # ── 4. Data manifest ───────────────────────────────────────────────
    data_manifest_schema = pa.schema([
        pa.field("status", pa.int32()),
        pa.field("file_path", pa.string()),
        pa.field("file_format", pa.string()),
        pa.field("content", pa.int32()),
        pa.field("record_count", pa.int64()),
        pa.field("file_size_in_bytes", pa.int64()),
    ])
    data_manifest_table = pa.table(
        {
            "status": [1],  # 1 = added
            "file_path": [data_path],
            "file_format": ["PARQUET"],
            "content": [0],  # 0 = data
            "record_count": [5],
            "file_size_in_bytes": [data_file_size],
        },
        schema=data_manifest_schema,
    )
    data_manifest_path = os.path.join(output_dir, "manifests", "data-manifest.parquet")
    pq.write_table(data_manifest_table, data_manifest_path)
    data_manifest_size = os.path.getsize(data_manifest_path)

    # ── 5. Positional delete manifest ──────────────────────────────────
    pos_del_manifest_table = pa.table(
        {
            "status": [1],
            "file_path": [pos_del_path],
            "file_format": ["PARQUET"],
            "content": [1],  # 1 = position_deletes
            "record_count": [1],
            "file_size_in_bytes": [pos_del_size],
        },
        schema=data_manifest_schema,
    )
    pos_del_manifest_path = os.path.join(
        output_dir, "manifests", "pos-del-manifest.parquet"
    )
    pq.write_table(pos_del_manifest_table, pos_del_manifest_path)
    pos_del_manifest_size = os.path.getsize(pos_del_manifest_path)

    # ── 6. Equality delete manifest ───────────────────────────────────
    eq_del_manifest_table = pa.table(
        {
            "status": [1],
            "file_path": [eq_del_path],
            "file_format": ["PARQUET"],
            "content": [2],  # 2 = equality_deletes
            "record_count": [1],
            "file_size_in_bytes": [eq_del_size],
        },
        schema=data_manifest_schema,
    )
    eq_del_manifest_path = os.path.join(
        output_dir, "manifests", "eq-del-manifest.parquet"
    )
    pq.write_table(eq_del_manifest_table, eq_del_manifest_path)
    eq_del_manifest_size = os.path.getsize(eq_del_manifest_path)

    # ── 7. Manifest list ───────────────────────────────────────────────
    manifest_list_schema = pa.schema([
        pa.field("manifest_path", pa.string()),
        pa.field("manifest_length", pa.int64()),
        pa.field("partition_spec_id", pa.int32()),
        pa.field("content", pa.int32()),
        pa.field("added_snapshot_id", pa.int64()),
        pa.field("added_data_files_count", pa.int32()),
        pa.field("existing_data_files_count", pa.int32()),
        pa.field("deleted_data_files_count", pa.int32()),
    ])
    manifest_list_table = pa.table(
        {
            "manifest_path": [
                data_manifest_path,
                pos_del_manifest_path,
                eq_del_manifest_path,
            ],
            "manifest_length": [
                data_manifest_size,
                pos_del_manifest_size,
                eq_del_manifest_size,
            ],
            "partition_spec_id": [0, 0, 0],
            "content": [0, 1, 1],  # 0=data, 1=deletes, 1=deletes
            "added_snapshot_id": [2000000000, 2000000000, 2000000000],
            "added_data_files_count": [1, 1, 1],
            "existing_data_files_count": [0, 0, 0],
            "deleted_data_files_count": [0, 0, 0],
        },
        schema=manifest_list_schema,
    )
    manifest_list_path = os.path.join(
        output_dir, "manifests", "snap-manifest-list.parquet"
    )
    pq.write_table(manifest_list_table, manifest_list_path)

    # ── 8. Metadata JSON ──────────────────────────────────────────────
    metadata = {
        "format-version": 2,
        "table-uuid": "test-iceberg-delete-table-uuid-0001",
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
        "current-snapshot-id": 2000000000,
        "snapshots": [
            {
                "snapshot-id": 2000000000,
                "timestamp-ms": 1700000000000,
                "manifest-list": manifest_list_path,
                "summary": {
                    "operation": "overwrite",
                    "added-data-files": "1",
                    "added-delete-files": "2",
                    "total-records": "5",
                },
            }
        ],
        "snapshot-log": [
            {"timestamp-ms": 1700000000000, "snapshot-id": 2000000000}
        ],
    }
    metadata_path = os.path.join(output_dir, "metadata", "v1.metadata.json")
    with open(metadata_path, "w") as f:
        json.dump(metadata, f, indent=2)

    print(f"Iceberg test data (with deletes) generated at: {output_dir}")
    print(f"  Data file:           {data_path}")
    print(f"  Positional delete:   {pos_del_path}")
    print(f"  Equality delete:     {eq_del_path}")
    print(f"  Expected result:     3 rows (id=1,3,5)")


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(
        os.path.abspath(__file__)
    )
    generate(out)
