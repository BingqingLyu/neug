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
import struct
import sys

import pyarrow as pa
import pyarrow.parquet as pq


# ── Iceberg single-value serialisation helpers ──────────────────────────

def _encode_long(v: int) -> bytes:
    return struct.pack("<q", v)

def _encode_double(v: float) -> bytes:
    return struct.pack("<d", v)

def _encode_string(v: str) -> bytes:
    return v.encode("utf-8")


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


def _write_manifest_file(
    output_dir: str,
    filename: str,
    data_file_entries: list,
):
    """Write a manifest file (Parquet) with lower/upper bounds.

    Each entry in *data_file_entries* is a dict:
        file_path, file_format, record_count, file_size_in_bytes,
        lower_bounds: {field_id: bytes}, upper_bounds: {field_id: bytes}
    """
    statuses, paths, formats = [], [], []
    record_counts, file_sizes = [], []
    lower_bounds_list, upper_bounds_list = [], []

    for e in data_file_entries:
        statuses.append(1)  # added
        paths.append(e["file_path"])
        formats.append(e.get("file_format", "PARQUET"))
        record_counts.append(e["record_count"])
        file_sizes.append(e["file_size_in_bytes"])
        lb = list(e.get("lower_bounds", {}).items())
        ub = list(e.get("upper_bounds", {}).items())
        lower_bounds_list.append(lb)
        upper_bounds_list.append(ub)

    map_type = pa.map_(pa.int32(), pa.binary())
    schema = pa.schema([
        pa.field("status", pa.int32()),
        pa.field("file_path", pa.string()),
        pa.field("file_format", pa.string()),
        pa.field("record_count", pa.int64()),
        pa.field("file_size_in_bytes", pa.int64()),
        pa.field("lower_bounds", map_type),
        pa.field("upper_bounds", map_type),
    ])

    table = pa.table({
        "status": statuses,
        "file_path": paths,
        "file_format": formats,
        "record_count": record_counts,
        "file_size_in_bytes": file_sizes,
        "lower_bounds": pa.array(lower_bounds_list, type=map_type),
        "upper_bounds": pa.array(upper_bounds_list, type=map_type),
    }, schema=schema)

    out_path = os.path.join(output_dir, "manifests", filename)
    pq.write_table(table, out_path)
    return out_path, os.path.getsize(out_path)


def _write_manifest_list(
    output_dir: str,
    manifests: list,
):
    """Write a manifest list with partition summaries.

    Each element of *manifests* is a dict:
        manifest_path, manifest_length, content,
        partitions: [{lower_bound: bytes, upper_bound: bytes}]
    """
    partition_summary_type = pa.struct([
        pa.field("contains_null", pa.bool_()),
        pa.field("lower_bound", pa.binary()),
        pa.field("upper_bound", pa.binary()),
    ])
    partitions_type = pa.list_(partition_summary_type)

    schema = pa.schema([
        pa.field("manifest_path", pa.string()),
        pa.field("manifest_length", pa.int64()),
        pa.field("partition_spec_id", pa.int32()),
        pa.field("content", pa.int32()),
        pa.field("added_snapshot_id", pa.int64()),
        pa.field("added_data_files_count", pa.int32()),
        pa.field("existing_data_files_count", pa.int32()),
        pa.field("deleted_data_files_count", pa.int32()),
        pa.field("partitions", partitions_type),
    ])

    paths, lengths, contents = [], [], []
    added_counts, partitions_list = [], []
    for m in manifests:
        paths.append(m["manifest_path"])
        lengths.append(m["manifest_length"])
        contents.append(m.get("content", 0))
        added_counts.append(m.get("added_data_files_count", 1))
        partitions_list.append([
            {
                "contains_null": False,
                "lower_bound": p["lower_bound"],
                "upper_bound": p["upper_bound"],
            }
            for p in m.get("partitions", [])
        ])

    table = pa.table({
        "manifest_path": paths,
        "manifest_length": lengths,
        "partition_spec_id": [0] * len(manifests),
        "content": contents,
        "added_snapshot_id": [2000000000] * len(manifests),
        "added_data_files_count": added_counts,
        "existing_data_files_count": [0] * len(manifests),
        "deleted_data_files_count": [0] * len(manifests),
        "partitions": pa.array(partitions_list, type=partitions_type),
    }, schema=schema)

    out_path = os.path.join(
        output_dir, "manifests", "snap-manifest-list.parquet"
    )
    pq.write_table(table, out_path)
    return out_path


def generate_partitioned(output_dir: str):
    """Generate a partitioned Iceberg table with multiple data files.

    Layout:
      category=A: file-A1 (id 1-3, value 10-30), file-A2 (id 4-6, value 40-60)
      category=B: file-B1 (id 7-9, value 70-90)

    Schema:  id LONG, name STRING, value DOUBLE, category STRING
    Partition spec:  category (identity)
    """
    os.makedirs(os.path.join(output_dir, "data"), exist_ok=True)
    os.makedirs(os.path.join(output_dir, "manifests"), exist_ok=True)
    os.makedirs(os.path.join(output_dir, "metadata"), exist_ok=True)

    data_schema = pa.schema([
        pa.field("id", pa.int64()),
        pa.field("name", pa.string()),
        pa.field("value", pa.float64()),
        pa.field("category", pa.string()),
    ])

    # ── Data files ──────────────────────────────────────────────────────
    files_info = []

    # File A1: category=A, value range [10.5, 30.7]
    t_a1 = pa.table({
        "id": [1, 2, 3],
        "name": ["Alice", "Bob", "Charlie"],
        "value": [10.5, 20.3, 30.7],
        "category": ["A", "A", "A"],
    }, schema=data_schema)
    p_a1 = os.path.join(output_dir, "data", "00000-0-data-A1.parquet")
    pq.write_table(t_a1, p_a1)
    files_info.append({
        "rel": "data/00000-0-data-A1.parquet",
        "size": os.path.getsize(p_a1),
        "count": 3,
        "category": "A",
        "id_lo": 1, "id_hi": 3,
        "val_lo": 10.5, "val_hi": 30.7,
    })

    # File A2: category=A, value range [40.1, 60.2]
    t_a2 = pa.table({
        "id": [4, 5, 6],
        "name": ["Diana", "Eve", "Frank"],
        "value": [40.1, 50.9, 60.2],
        "category": ["A", "A", "A"],
    }, schema=data_schema)
    p_a2 = os.path.join(output_dir, "data", "00000-1-data-A2.parquet")
    pq.write_table(t_a2, p_a2)
    files_info.append({
        "rel": "data/00000-1-data-A2.parquet",
        "size": os.path.getsize(p_a2),
        "count": 3,
        "category": "A",
        "id_lo": 4, "id_hi": 6,
        "val_lo": 40.1, "val_hi": 60.2,
    })

    # File B1: category=B, value range [70.4, 90.8]
    t_b1 = pa.table({
        "id": [7, 8, 9],
        "name": ["Grace", "Heidi", "Ivan"],
        "value": [70.4, 80.6, 90.8],
        "category": ["B", "B", "B"],
    }, schema=data_schema)
    p_b1 = os.path.join(output_dir, "data", "00000-2-data-B1.parquet")
    pq.write_table(t_b1, p_b1)
    files_info.append({
        "rel": "data/00000-2-data-B1.parquet",
        "size": os.path.getsize(p_b1),
        "count": 3,
        "category": "B",
        "id_lo": 7, "id_hi": 9,
        "val_lo": 70.4, "val_hi": 90.8,
    })

    # Field IDs:  id=1, name=2, value=3, category=4
    def _make_bounds(info):
        return {
            "lower_bounds": {
                1: _encode_long(info["id_lo"]),
                3: _encode_double(info["val_lo"]),
                4: _encode_string(info["category"]),
            },
            "upper_bounds": {
                1: _encode_long(info["id_hi"]),
                3: _encode_double(info["val_hi"]),
                4: _encode_string(info["category"]),
            },
        }

    # ── Manifest files ──────────────────────────────────────────────────
    # manifest-A: files for category=A
    mf_a_entries = []
    for fi in files_info:
        if fi["category"] == "A":
            bounds = _make_bounds(fi)
            mf_a_entries.append({
                "file_path": fi["rel"],
                "record_count": fi["count"],
                "file_size_in_bytes": fi["size"],
                **bounds,
            })
    _, mf_a_size = _write_manifest_file(output_dir, "manifest-A.parquet", mf_a_entries)

    # manifest-B: files for category=B
    mf_b_entries = []
    for fi in files_info:
        if fi["category"] == "B":
            bounds = _make_bounds(fi)
            mf_b_entries.append({
                "file_path": fi["rel"],
                "record_count": fi["count"],
                "file_size_in_bytes": fi["size"],
                **bounds,
            })
    _, mf_b_size = _write_manifest_file(output_dir, "manifest-B.parquet", mf_b_entries)

    # ── Manifest list ───────────────────────────────────────────────────
    _write_manifest_list(output_dir, [
        {
            "manifest_path": "manifests/manifest-A.parquet",
            "manifest_length": mf_a_size,
            "added_data_files_count": len(mf_a_entries),
            "partitions": [{"lower_bound": _encode_string("A"),
                            "upper_bound": _encode_string("A")}],
        },
        {
            "manifest_path": "manifests/manifest-B.parquet",
            "manifest_length": mf_b_size,
            "added_data_files_count": len(mf_b_entries),
            "partitions": [{"lower_bound": _encode_string("B"),
                            "upper_bound": _encode_string("B")}],
        },
    ])

    # ── Metadata JSON ───────────────────────────────────────────────────
    metadata = {
        "format-version": 1,
        "table-uuid": "test-iceberg-partitioned-uuid-0001",
        "location": output_dir,
        "schema": {
            "type": "struct",
            "fields": [
                {"id": 1, "name": "id", "required": False, "type": "long"},
                {"id": 2, "name": "name", "required": False, "type": "string"},
                {"id": 3, "name": "value", "required": False, "type": "double"},
                {"id": 4, "name": "category", "required": False, "type": "string"},
            ],
        },
        "partition-spec": [
            {
                "source-id": 4,
                "field-id": 1000,
                "name": "category",
                "transform": "identity",
            }
        ],
        "default-spec-id": 0,
        "last-partition-id": 1000,
        "properties": {},
        "current-snapshot-id": 2000000000,
        "snapshots": [
            {
                "snapshot-id": 2000000000,
                "timestamp-ms": 1700000000000,
                "manifest-list": "manifests/snap-manifest-list.parquet",
                "summary": {
                    "operation": "append",
                    "added-data-files": "3",
                    "total-records": "9",
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

    print(f"Partitioned Iceberg test data generated at: {output_dir}")
    print(f"  Data files:     3 (A1, A2, B1)")
    print(f"  Manifests:      2 (A, B)")
    print(f"  Metadata:       {metadata_path}")


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "simple"
    default_dir = os.path.dirname(os.path.abspath(__file__))
    if mode == "partitioned":
        out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(default_dir, "partitioned")
        generate_partitioned(out)
    else:
        out = sys.argv[2] if len(sys.argv) > 2 else default_dir
        generate(out)
