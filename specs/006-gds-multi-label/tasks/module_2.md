# Module 2: GDS Dispatch Layer Integration

**Goal**: Wire MergedStorageView into the GDS algorithm execution path so that multi-label projections are transparently consumed by all algorithms, with correct result unmapping.

**Assignee**: BingqingLyu
**Label**: enhancement
**Milestone**: v0.2.0
**Project**: neug

## [F006-T201] Store multi-label metadata in algorithm input

**description**: Extend the GDS algorithm bind path to detect multi-label subgraphs and store the label/triplet metadata needed for MergedStorageView construction at exec time.

**details**:
* File: `src/execution/execute/ops/retrieve/gds_algo.cc` and its header
* Add a new struct `MultiLabelMeta` to hold:
  - `std::vector<label_t> vertex_labels`
  - `std::vector<execution::LabelTriplet> edge_triplets`
  - `bool is_multi_label` flag
* Modify `GDSAlgoOprBuilder::Build()`:
  - After calling `algo_func->bindFunc(...)`, inspect the parsed `Subgraph` proto
  - If `vertex_entries_size() > 1` OR any edge_entry has `src_label != dst_label` OR multiple edge entries exist:
    - Extract all vertex labels and edge triplets
    - Store in `MultiLabelMeta` alongside `algo_input_`
  - Otherwise set `is_multi_label = false`
* Store `MultiLabelMeta` as a member of `GDSAlgoOpr` (alongside `algo_input_`)
* Note: For multi-label case, each algorithm's `bind()` still needs to succeed. This means we need to bypass `check_simple_graph_subgraph` first (depends on Module 3 T301). For now, pass a dummy single label to the algorithm's bind, and the real multi-label info goes into `MultiLabelMeta`.

## [F006-T202] Insert MergedStorageView dispatch in GDSAlgoOpr::Eval

**description**: Modify the exec entry point to construct MergedStorageView when multi-label is detected, and pass it to the algorithm instead of the raw storage interface.

**details**:
* File: `src/execution/execute/ops/retrieve/gds_algo.cc`
* In `GDSAlgoOpr::Eval()`:
  ```
  if (multi_label_meta_.is_multi_label) {
    auto& base_graph = dynamic_cast<StorageReadInterface&>(graph_interface);
    MergedStorageView merged(base_graph, 
                             multi_label_meta_.vertex_labels,
                             multi_label_meta_.edge_triplets);
    auto result = algo_func_->execFunc(*algo_input_, merged);
    // unmapping happens in T203
    return unmap_result(result, merged.offset_table());
  } else {
    return algo_func_->execFunc(*algo_input_, graph_interface);
  }
  ```
* Add `#include "utils/merged_storage_view.h"` (need to ensure the GDS extension header is accessible from core execution code, OR move the dispatch into the extension side)
* Alternative: if header dependency is problematic, define a factory function in the GDS extension that the core code calls via function pointer. Evaluate during implementation.
* Single-label path: ZERO changes, same as before.

## [F006-T203] Implement result vertex unmapping

**description**: After algorithm execution on a merged view, translate output vertex columns from global unified IDs back to proper (label, local_vid) vertex references.

**details**:
* The algorithm's output `execution::Context` contains vertex columns with global vid values
* After exec returns, iterate the vertex column:
  - For each global_vid, call `offset_table.global_to_local(global_vid)` to get `(label_idx, local_vid)`
  - Get the actual `label_t` from `offset_table.range(label_idx).label`
  - Reconstruct the vertex value as `(label, local_vid)` pair
* This unmapping runs once per result row (typically O(V) total)
* File: add unmapping logic in `gds_algo.cc` (inline in the multi-label branch of Eval) or as a helper function
* Vertex columns use `VertexColumnBuilder` — inspect its API to determine how to rebuild the column with corrected (label, vid) pairs
* Edge case: if algorithm doesn't output a vertex column (unlikely but possible), skip unmapping

## [F006-T204] Integration test: multi-label Leiden/WCC/PageRank

**description**: End-to-end Python tests that run GDS algorithms on multi-label projected graphs and verify correctness.

**details**:
* File: `tools/python_bind/tests/test_gds_multi_label.py` (new)
* Test setup:
  - Create a graph with: Person vertices (id: 1-5), TEMP_Person vertices (id: 1-3)
  - Edges: Person-KNOWS-Person (some edges), Person-KNOWS-TEMP_Person (some edges)
  - Use COPY TEMP to load TEMP_Person vertices
* Test cases:
  1. **WCC**: `project_graph('g', ['Person', 'TEMP_Person'], {['Person','KNOWS','Person']:'', ['Person','KNOWS','TEMP_Person']:''})`; run WCC; verify all connected vertices share same component ID regardless of label
  2. **PageRank**: Run PageRank on the same multi-label graph; verify result includes both Person and TEMP_Person vertices; verify rank sum ≈ 1.0
  3. **Leiden**: Run Leiden; verify community assignments cover all vertices from both labels
  4. **Single-label baseline**: Same algorithms on single-label projection produce identical results to current behavior
* Verify: `YIELD node` returns proper vertex references (not raw global vids)
* Mark as extension test (requires GDS extension loaded)
