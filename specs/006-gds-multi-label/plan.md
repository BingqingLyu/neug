# Implementation Plan: GDS Multi-Label Graph Projection

**Branch**: `006-gds-multi-label` | **Date**: 2026-07-03 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `specs/006-gds-multi-label/spec.md`

## Summary

Enable GDS algorithms to operate on projected graphs with multiple vertex labels and edge triplets. The approach is to:
1. Make key `StorageReadInterface` methods virtual
2. Implement `MergedStorageView` that constructs a merged adjacency index at exec time
3. Insert the view transparently in the GDS dispatch layer when multi-label subgraphs are detected
4. Relax `check_simple_graph_subgraph` to accept homogeneous multi-label projections

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: neug core library (StorageReadInterface, CsrView, VertexSet), GDS extension  
**Storage**: CSR-based graph storage with per-label vertex tables and per-triplet CSR adjacency  
**Testing**: Python pytest (`tests/test_gds.py`), C++ gtest (`extension/gds/test/`)

## Project Structure

```text
extension/gds/
├── include/
│   ├── utils/
│   │   ├── subgraph_utils.h          # [MODIFY] add check_homogeneous_subgraph
│   │   └── merged_storage_view.h     # [NEW] MergedStorageView class
│   └── ...                            # existing algo headers (unchanged)
├── src/
│   ├── utils/
│   │   ├── subgraph_utils.cc         # [MODIFY] implement check_homogeneous_subgraph
│   │   └── merged_storage_view.cc    # [NEW] MergedStorageView implementation
│   └── ...                            # existing algo sources (unchanged)
└── test/
    └── merged_view_test.cc            # [NEW] unit tests for MergedStorageView

include/neug/storages/graph/
└── graph_interface.h                  # [MODIFY] make 3 methods virtual

src/execution/execute/ops/retrieve/
└── gds_algo.cc                        # [MODIFY] insert MergedStorageView dispatch

tools/python_bind/tests/
└── test_gds_multi_label.py            # [NEW] integration tests
```

**Structure Decision**: New files concentrated in `extension/gds/` (view implementation) with minimal changes to core (`graph_interface.h` adds `virtual` keyword, `gds_algo.cc` adds dispatch logic).

## Data Model

This feature has the following data models:
1. LabelOffsetTable — maps each vertex label to its offset in the unified vid space
2. MergedAdjacencyIndex — in-memory edge index for the unified graph
3. MergedVertexTimestamp — validity bitmap for the unified vertex set

### LabelOffsetTable

**Data Structure**:

A small fixed-size array (typically 2-5 entries) that maps label position to its global vid offset.

```
Example: Person has 1000 vertices (max_vid=999), TEMP_Person has 500 vertices (max_vid=499)

label_ranges = [
  { label: 0 (Person),      offset: 0,    capacity: 1000 },
  { label: 1 (TEMP_Person), offset: 1000, capacity: 500  }
]
total_capacity = 1500
```

**Data Access & Update**:

- **local_to_global(label_idx, local_vid)**: returns `offset[label_idx] + local_vid`. O(1).
- **global_to_local(global_vid)**: binary search in offsets array to find which label range contains the global_vid, return `(label_idx, global_vid - offset[label_idx])`. O(log L) where L = number of labels (typically 2-5, effectively O(1)).
- **get_total_capacity()**: returns `total_capacity`. Used for array sizing in algorithms.

### MergedAdjacencyIndex

**Data Structure**:

A flat array of neighbor entries indexed by global vid. For each vertex in the unified space, stores its concatenated outgoing (and incoming) neighbors from all relevant edge triplets, with vid values already mapped to global space.

```
Example: Person vid=3 has outgoing KNOWS edges to Person {7, 12} and to TEMP_Person {2, 5}

After merging (TEMP_Person offset = 1000):
  merged_out_neighbors[3] = [7, 12, 1002, 1005]
  merged_out_degrees[3] = 4
```

Storage layout:
- `degrees_out[total_capacity]` — degree array for outgoing
- `degrees_in[total_capacity]` — degree array for incoming
- `adjlist_out[total_capacity]` — pointer to start of each vertex's outgoing neighbor list
- `adjlist_in[total_capacity]` — pointer to start of each vertex's incoming neighbor list
- `nbr_data_out[total_edges_out]` — flat buffer of all outgoing neighbor entries (vid_t + optional edge data)
- `nbr_data_in[total_edges_in]` — flat buffer of all incoming neighbor entries

**Data Access & Update**:

- **Construction**: At MergedStorageView creation, iterate all edge triplets. For each triplet (src_label, edge_label, dst_label):
  1. Get the CsrView from the underlying StorageReadInterface
  2. For each valid vertex in src_label's vertex set, enumerate neighbors
  3. Apply offset mapping to both the source vid and each neighbor vid
  4. Append to the flat neighbor buffer at the appropriate position
- **get_edges(global_vid)**: Return a NbrList pointing to `adjlist[global_vid]` with degree `degrees[global_vid]`. O(1). This is a standard CsrView operation.
- **No updates**: The index is immutable after construction, lives for the duration of one algorithm execution.

### MergedVertexTimestamp

**Data Structure**:

A lightweight validity marker that makes ALL global vids from 0 to total_capacity-1 valid. This is needed because `VertexSet` requires a `VertexTimestamp&` reference.

```
Implementation: A VertexTimestamp where IsVertexValid(v, ts) = true for all v < total_capacity.
Alternatively: Store a vector<vid_t> of all valid vertices and provide iteration directly.
```

**Data Access & Update**:

- **iteration**: Yields all valid global vids (union of per-label valid vertex sets with offset applied). Used by algorithms to enumerate all vertices.
- **size()**: Returns total_capacity. Used by algorithms for array allocation.
- **valid(v)**: Returns true if v corresponds to a valid vertex in any label's vertex set. O(1) via lookup in the offset table + delegate to underlying VertexSet.

## Algorithm Model

This feature has the following algorithm models:
1. Offset Computation
2. Adjacency Materialization
3. Multi-Label Dispatch Decision

### Offset Computation

**Algorithm Target**: Compute the global vid offset for each vertex label so that vid spaces don't overlap.

**Algorithm Details**:

```
Input: vertex_labels = [label_0, label_1, ..., label_k]
       graph: StorageReadInterface

Algorithm:
  offset = 0
  for each label_i in vertex_labels:
    vertex_set_i = graph.GetVertexSet(label_i)
    capacity_i = vertex_set_i.size()     // max vid + 1 for this label
    label_ranges[i] = { label: label_i, offset: offset, capacity: capacity_i }
    offset += capacity_i
  total_capacity = offset

Output: label_ranges[], total_capacity
```

Example:
- Person: capacity=1000 → offset=0, range=[0, 1000)
- TEMP_Person: capacity=500 → offset=1000, range=[1000, 1500)
- total_capacity=1500

### Adjacency Materialization

**Algorithm Target**: Build a merged CsrView-compatible adjacency index from multiple edge triplets.

**Algorithm Details**:

```
Input: edge_triplets = [(src_label, dst_label, edge_label), ...]
       label_ranges (from offset computation)
       graph: StorageReadInterface

Phase 1 — Count degrees:
  Initialize degrees_out[total_capacity] = 0
  For each triplet (src_l, dst_l, e_l):
    oe_view = graph.GetGenericOutgoingGraphView(src_l, dst_l, e_l)
    For each valid vertex v in graph.GetVertexSet(src_l):
      global_v = local_to_global(src_l, v)
      count = count_edges(oe_view.get_edges(v))
      degrees_out[global_v] += count

Phase 2 — Allocate flat buffer:
  total_edges = sum(degrees_out[0..total_capacity-1])
  Allocate nbr_buffer[total_edges] (each entry is one stride worth of data)
  Compute prefix-sum of degrees to get adjlist_out pointers

Phase 3 — Fill neighbor data:
  For each triplet (src_l, dst_l, e_l):
    oe_view = graph.GetGenericOutgoingGraphView(src_l, dst_l, e_l)
    dst_offset = label_ranges[dst_l].offset
    For each valid vertex v in graph.GetVertexSet(src_l):
      global_v = local_to_global(src_l, v)
      For each neighbor n in oe_view.get_edges(v):
        global_n = n + dst_offset
        Write global_n to next position in adjlist_out[global_v]

  (Repeat symmetrically for incoming edges if needed)

Output: CsrView pointing to the materialized buffer
```

Example with Person(0-999) and TEMP_Person(1000-1499):
- Triplet (Person,KNOWS,Person): Person vid=3 → neighbors {7, 12} → global {7, 12}
- Triplet (Person,KNOWS,TEMP_Person): Person vid=3 → neighbors {2, 5} → global {1002, 1005}
- Merged result: global_v=3 → neighbors [7, 12, 1002, 1005], degree=4

### Multi-Label Dispatch Decision

**Algorithm Target**: Determine at runtime whether to construct a MergedStorageView or use the direct path.

**Algorithm Details**:

```
Input: ParsedSubgraph (from physical plan), StorageReadInterface

Decision:
  IF vertex_entries.size() == 1 AND edge_entries.size() == 1
     AND edge_entries[0].src_label == vertex_entries[0].label
     AND edge_entries[0].dst_label == vertex_entries[0].label:
    → Single-label: pass StorageReadInterface directly to algorithm (ZERO overhead)
  
  ELSE IF all edge_entry endpoints are in vertex_entries label set:
    → Multi-label homogeneous: construct MergedStorageView, pass to algorithm
  
  ELSE:
    → Error: "Edge triplet references label not in vertex set"

Insertion point: GDSAlgoOpr::Eval() in gds_algo.cc
  Before: return algo_func_->execFunc(*algo_input_, graph_interface);
  After:
    if (is_multi_label_) {
      MergedStorageView merged(graph_interface, label_ranges_, edge_triplets_);
      return algo_func_->execFunc(*algo_input_, merged);
    } else {
      return algo_func_->execFunc(*algo_input_, graph_interface);
    }
```

Note: The multi-label metadata (label_ranges, edge_triplets) is computed at bind time and stored in the algo_input. The view is constructed fresh at exec time (per-query lifecycle).

## Key Design Decisions

1. **Virtual methods on StorageReadInterface**: Making `GetVertexSet()`, `GetGenericOutgoingGraphView()`, `GetGenericIncomingGraphView()` virtual is required because algorithms receive the interface by reference and call these methods directly. The vtable overhead is negligible and only applies to the multi-label path.

2. **Adjacency materialization is acceptable**: Although the spec says "no data copy", this refers to not copying the underlying property graph store. Building a lightweight edge index (offsets + remapped vid_t values) at exec time is necessary because CsrView requires contiguous memory. The index has O(V+E) construction cost and is discarded after algorithm completion.

3. **Single-label zero-overhead guarantee**: The dispatch logic is a simple branch at the start of `Eval()`. When the subgraph is single-label (the common case), execution proceeds exactly as before — no MergedStorageView is constructed, no virtual dispatch occurs.

4. **Result unmapping**: Algorithm output contains global vids. The dispatch layer maps them back to (label, local_vid) pairs using the LabelOffsetTable before returning results to the query engine.
