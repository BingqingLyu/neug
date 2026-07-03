# Module 1: Unified Graph View

**Goal**: Provide a runtime MergedStorageView that presents multiple vertex label sets and edge triplets as a single homogeneous graph, usable by all existing GDS algorithms without modification.

**Assignee**: BingqingLyu
**Label**: enhancement
**Milestone**: v0.2.0
**Project**: neug

## [F006-T101] Make StorageReadInterface methods virtual

**description**: Add `virtual` keyword to three methods in `StorageReadInterface` so that `MergedStorageView` can override them. This is the minimal prerequisite for polymorphic dispatch.

**details**:
* File: `include/neug/storages/graph/graph_interface.h`
* Make these methods virtual:
  - `GetVertexSet(label_t label)` → `virtual VertexSet GetVertexSet(label_t label) const`
  - `GetGenericOutgoingGraphView(label_t, label_t, label_t)` → `virtual CsrView GetGenericOutgoingGraphView(...) const`
  - `GetGenericIncomingGraphView(label_t, label_t, label_t)` → `virtual CsrView GetGenericIncomingGraphView(...) const`
* Add `virtual ~StorageReadInterface() {}` if not already present (ensure polymorphic destruction)
* Verify the existing tests still pass (no behavioral change, only vtable addition)
* No other code changes needed — the base class implementations remain the default

## [F006-T102] Implement LabelOffsetTable

**description**: Create the offset computation data structure that maps each vertex label to its position in the unified vid space.

**details**:
* File: `extension/gds/include/utils/merged_storage_view.h` (new file, struct definition)
* Define `struct LabelRange { label_t label; vid_t offset; vid_t capacity; };`
* Define `class LabelOffsetTable` with:
  - Constructor: takes `const StorageReadInterface& graph, const std::vector<label_t>& vertex_labels`
  - Iterates each label, calls `graph.GetVertexSet(label).size()` to get capacity
  - Computes cumulative offset
  - Stores `std::vector<LabelRange> ranges_` and `vid_t total_capacity_`
* Methods:
  - `vid_t local_to_global(size_t label_idx, vid_t local_vid) const` → return `ranges_[label_idx].offset + local_vid`
  - `std::pair<size_t, vid_t> global_to_local(vid_t global_vid) const` → binary search in ranges (or linear scan since L≤5)
  - `vid_t total_capacity() const`
  - `size_t label_count() const`
  - `const LabelRange& range(size_t idx) const`

## [F006-T103] Implement MergedStorageView with adjacency materialization

**description**: Implement the core `MergedStorageView` class that constructs a merged adjacency index at creation time and overrides `StorageReadInterface` methods to serve the unified graph.

**details**:
* Files:
  - `extension/gds/include/utils/merged_storage_view.h` (class declaration)
  - `extension/gds/src/utils/merged_storage_view.cc` (implementation)
* `MergedStorageView` inherits from `StorageReadInterface`
* Constructor parameters: `const StorageReadInterface& base_graph, const std::vector<label_t>& vertex_labels, const std::vector<execution::LabelTriplet>& edge_triplets`
* Construction steps:
  1. Build `LabelOffsetTable` from vertex_labels
  2. Allocate `degrees_out_[total_capacity]` and `degrees_in_[total_capacity]` (zero-initialized)
  3. First pass: count degrees by iterating all triplets and counting edges per global vertex
  4. Prefix-sum to compute adjacency pointers (`adjlist_ptrs_out_`, `adjlist_ptrs_in_`)
  5. Allocate flat neighbor buffer `nbr_buf_out_[total_out_edges]` (each entry: `vid_t` neighbor)
  6. Second pass: fill neighbor buffers with offset-mapped neighbor IDs
  7. Build internal `CsrView` objects pointing to the materialized buffers
* Override methods:
  - `GetVertexSet(label_t)`: return a `VertexSet` that iterates all valid global vids. Use a custom `VertexTimestamp` owned by MergedStorageView where all vids in [0, total_capacity) that correspond to valid vertices in any label are marked valid.
  - `GetGenericOutgoingGraphView(...)`: return the materialized outgoing CsrView (ignore label params — all merged)
  - `GetGenericIncomingGraphView(...)`: return the materialized incoming CsrView
* Handle edge case: vertex label with zero vertices (capacity=0, contributes nothing)
* Store `LabelOffsetTable` as member for later result unmapping
* Must not outlive the base `StorageReadInterface` reference

## [F006-T104] Unit test MergedStorageView

**description**: Write C++ unit tests verifying offset computation, vertex set union, edge traversal correctness, and reverse mapping.

**details**:
* File: `extension/gds/test/merged_view_test.cc`
* Test cases:
  1. **Offset computation**: 2 labels with known capacities, verify offset and total_capacity
  2. **Vertex set union**: Construct MergedStorageView, iterate GetVertexSet, verify all expected global vids present
  3. **Edge traversal**: Set up a graph with Person(0-2) KNOWS Person(1,2) and Person(0) KNOWS TEMP_Person(0). Verify merged edges from global_v=0 include both Person and TEMP_Person neighbors with correct global IDs
  4. **Reverse mapping**: Verify global_to_local correctly resolves global vids to (label_idx, local_vid)
  5. **Single-label degenerate**: Verify that a single-label MergedStorageView behaves identically to direct access
  6. **Empty label**: One label has zero vertices — verify no crash, correct total_capacity
* Use GDS extension test infrastructure (gtest, add to CMakeLists.txt)
