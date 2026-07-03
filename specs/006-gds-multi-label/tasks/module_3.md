# Module 3: Subgraph Validation Relaxation

**Goal**: Replace the strict `check_simple_graph_subgraph` with a permissive `check_homogeneous_subgraph` that accepts multi-label projections where all edge endpoints are within the vertex label set, while rejecting truly heterogeneous or predicate-bearing multi-label subgraphs.

**Assignee**: BingqingLyu
**Label**: enhancement
**Milestone**: v0.2.0
**Project**: neug

## [F006-T301] Implement check_homogeneous_subgraph

**description**: Add a new validation function that accepts multi-label subgraphs as long as all edge triplet endpoints are within the declared vertex label set.

**details**:
* Files:
  - `extension/gds/include/utils/subgraph_utils.h` (declaration)
  - `extension/gds/src/utils/subgraph_utils.cc` (implementation)
* Add function: `bool check_homogeneous_subgraph(const ParsedSubgraph& parsed, const std::string& algo_name)`
* Logic:
  1. Reject if `vertex_entries.size() == 0` or `edge_entries.size() == 0`
  2. Build a set of declared vertex labels from `vertex_entries`
  3. For each edge entry, check that `triplet.src_label` and `triplet.dst_label` are in the vertex label set
  4. If any endpoint is missing: `LOG(ERROR) << "label X is not in the declared vertex label set for " << algo_name; return false;`
  5. Otherwise return true
* Modify all algorithm `parse_subgraph()` methods (leiden.cc, louvain.cc, page_rank.cc, wcc.cc, bfs.cc, sssp.cc, kcore.cc, lcc.cc, cdlp.cc):
  - Replace `check_simple_graph_subgraph(parsed, name)` with `check_homogeneous_subgraph(parsed, name)`
  - For algorithms that currently extract `vertex_label = parsed.vertex_entries[0].label` and `edge_label = parsed.edge_entries[0].triplet.edge_label`: keep this logic for the single-label case (it still works). For multi-label case, the actual merging is handled by the dispatch layer (Module 2), so these fields just need a valid default (first label is fine).
* Keep `check_simple_graph_subgraph` available (don't delete) in case other code uses it
* Important: This change alone doesn't make multi-label work end-to-end — it just removes the rejection gate. Full functionality requires Module 1 + Module 2.

## [F006-T302] Add predicate rejection for multi-label

**description**: When a multi-label subgraph is detected AND any vertex/edge entry has a non-null predicate, reject with a clear error message.

**details**:
* File: `extension/gds/src/utils/subgraph_utils.cc` (in `check_homogeneous_subgraph` or as a separate helper)
* Logic: After homogeneity check passes, if `vertex_entries.size() > 1` (multi-label detected):
  - Check all vertex_entries: if any has `predicate != nullptr`, error
  - Check all edge_entries: if any has `predicate != nullptr`, error
  - Error message: "Predicates are not supported in multi-label projections"
* Single-label subgraphs: do NOT add this restriction (they continue to support predicates as before)
* This implements the spec edge case decision: first version doesn't support predicates + multi-label combination

## [F006-T303] Integration test: validation acceptance/rejection

**description**: Python integration tests verifying that valid multi-label projections are accepted and invalid ones are rejected with proper error messages.

**details**:
* File: `tools/python_bind/tests/test_gds_multi_label.py` (append to the file from T204)
* Test cases:
  1. **Valid multi-label**: `project_graph('g', ['Person', 'TEMP_Person'], {['Person','KNOWS','Person']:'', ['Person','KNOWS','TEMP_Person']:''})`; run any algorithm → succeeds
  2. **Invalid heterogeneous**: `project_graph('g', ['Person'], {['Person','KNOWS','TEMP_Person']:''})`; run algorithm → error containing "not in the declared vertex label set"
  3. **Predicate + multi-label rejected**: `project_graph('g', ['Person', 'TEMP_Person'], {['Person','KNOWS','Person']:'n.age > 20'})`; run algorithm → error containing "Predicates are not supported in multi-label"
  4. **Backward compatible single-label**: `project_graph('g', ['Person'], {['Person','KNOWS','Person']:''})`; run algorithm → same behavior as before
  5. **Single-label with predicate still works**: `project_graph('g', ['Person'], {['Person','KNOWS','Person']:'n.age > 20'})`; run WCC → succeeds (no regression)
* Each error test: use `pytest.raises` or equivalent to verify exception message content
