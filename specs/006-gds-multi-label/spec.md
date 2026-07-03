# Feature Specification: GDS Multi-Label Graph Projection

**Feature Branch**: `006-gds-multi-label`  
**Created**: 2026-07-03  
**Status**: Draft  
**Input**: Enable GDS algorithms to operate on projected graphs with multiple vertex labels and edge triplets without algorithm code changes.

## Functional Modules *(mandatory)*

### Module 1: Unified Graph View (Priority: P1)

**Purpose**: Provide a runtime graph view abstraction that presents multiple vertex label sets and edge triplets as a single homogeneous graph. This is the core building block that enables algorithms to operate on multi-label projected graphs without knowing about the underlying label structure.

**Why this priority**: Without this view, no algorithm can consume multi-label projections. It is the prerequisite for all other modules.

**Independent Test**: Can be tested by constructing a view over a graph with 2+ vertex labels and verifying that: vertex enumeration returns the union of all labeled vertex sets with non-conflicting IDs, and edge traversal from any vertex returns the union of neighbors across all relevant edge triplets.

**Key Components**:

1. **Vertex Set Merger**: Combines multiple per-label vertex sets into one contiguous ID space using a deterministic offset scheme. Each label's vertices retain their relative order; labels are stacked sequentially to produce globally unique IDs.
2. **Edge Union Iterator**: For a given vertex, queries all edge triplets that originate from (or arrive at) that vertex's original label, applies the offset mapping to returned neighbor IDs, and presents the union as a single neighbor set.
3. **Reverse Mapping**: Translates a global vertex ID back to the original (label, local_vid) pair for result output.

**Functional Requirements**:

1. **FR-001**: The system MUST present a unified vertex set that is the union of all vertex sets across the specified labels, with no duplicate or conflicting IDs.
2. **FR-002**: The system MUST ensure edge traversal from any vertex returns the union of outgoing (and/or incoming) edges across all matching edge triplets, with neighbor IDs in the unified space.
3. **FR-003**: The system MUST NOT copy or materialize any underlying graph data—the view is purely referential.
4. **FR-004**: The system MUST support mapping any unified ID back to its original (label, local_vid) pair in O(1) or O(log N) time, where N is the number of labels (not vertices).
5. **FR-005**: The system MUST handle the case where only one vertex label and one edge triplet are specified, behaving identically to the current single-label path (zero overhead).

**Acceptance Scenarios**:

1. **Given** a graph with Person (vids 0-999) and TEMP_Person (vids 0-499), **When** a unified view is constructed over both labels, **Then** the unified vertex set contains 1500 unique IDs and Person vid=5 and TEMP_Person vid=5 are distinguishable.
2. **Given** edge triplets (Person,KNOWS,Person) and (Person,KNOWS,TEMP_Person), **When** traversing outgoing edges from Person vid=3, **Then** the returned neighbor set includes neighbors from both triplets with correctly mapped unified IDs.
3. **Given** an algorithm produces a result for unified ID=1005, **When** reverse-mapping is applied, **Then** it resolves to (TEMP_Person, local_vid=5).

**Test Strategy**:

- **Unit Tests**: Verify offset computation correctness; verify union traversal completeness (no missed edges, no duplicates); verify reverse mapping accuracy; verify single-label degenerate case has no overhead.
- **Integration Tests**: Run with a real multi-label graph in storage and verify all vertices and edges are reachable through the view.

---

### Module 2: GDS Dispatch Layer Integration (Priority: P2)

**Purpose**: Modify the GDS algorithm dispatch path so that when a projected subgraph contains multiple vertex labels or edge triplets, the unified view is transparently constructed and provided to the algorithm. No algorithm implementation code is modified.

**Why this priority**: Depends on Module 1 (the view itself). This module is the "wiring" that makes multi-label projections usable by all existing algorithms without per-algorithm changes.

**Independent Test**: Can be tested by calling any GDS algorithm (e.g., WCC or PageRank) with a multi-label projected graph and verifying it produces results covering vertices from all specified labels, without any algorithm code changes.

**Key Components**:

1. **Multi-Label Detection**: At the point where the algorithm receives the parsed `Subgraph` proto, determine whether the subgraph qualifies for unified view construction (multiple vertex entries, or edge entries with differing src/dst labels).
2. **View Construction Trigger**: When multi-label is detected, instantiate the unified view (Module 1) wrapping the underlying `StorageReadInterface`, and pass it to the algorithm's exec function instead of the raw storage interface.
3. **Result Unmapping**: After algorithm execution, translate output vertex references from unified IDs back to their original (label, local_vid) representation for correct downstream consumption.

**Functional Requirements**:

1. **FR-006**: The system MUST automatically detect multi-label subgraph projections and apply the unified view without any explicit user action beyond the existing `project_graph()` call.
2. **FR-007**: The system MUST pass the unified view to algorithms through the existing `StorageReadInterface` contract, requiring zero changes to algorithm `exec()` implementations.
3. **FR-008**: The system MUST correctly translate algorithm output vertices from unified IDs back to labeled vertices for query result consumption.
4. **FR-009**: When the subgraph contains exactly one vertex label and one edge triplet (current behavior), the system MUST NOT introduce any additional overhead—the dispatch path remains as-is.

**Acceptance Scenarios**:

1. **Given** `project_graph('g', ['Person', 'TEMP_Person'], {['Person','KNOWS','Person']:'', ['Person','KNOWS','TEMP_Person']:''})`, **When** `CALL leiden('g', ...) YIELD node, community`, **Then** the result includes rows for both Person and TEMP_Person vertices with valid community assignments.
2. **Given** the same multi-label projection, **When** `CALL page_rank('g', ...) YIELD node, rank`, **Then** PageRank values are computed considering edges across both triplets, without modifying `page_rank.cc`.
3. **Given** `project_graph('g', ['Person'], {['Person','KNOWS','Person']:''})` (single-label), **When** any algorithm runs, **Then** behavior and performance are identical to the current implementation.

**Test Strategy**:

- **Unit Tests**: Verify detection logic correctly identifies multi-label vs. single-label subgraphs; verify the view is only constructed when needed.
- **Integration Tests**: Run Leiden, WCC, PageRank on multi-label projected graphs and verify correctness of results compared to manual computation.

---

### Module 3: Subgraph Validation Relaxation (Priority: P2)

**Purpose**: Replace the current strict `check_simple_graph_subgraph` validation (which rejects any subgraph with more than one vertex label or edge triplet) with a more permissive check that allows multi-label projections as long as they are semantically homogeneous (all edge triplets connect vertices within the specified label set).

**Why this priority**: Same priority as Module 2 because without relaxing validation, multi-label subgraphs are rejected before reaching the dispatch layer. Must be coordinated with Module 2.

**Independent Test**: Can be tested by verifying that `project_graph()` with multiple homogeneous labels passes validation, while truly heterogeneous projections (e.g., edges connecting to labels NOT in the vertex set) are still rejected with a clear error.

**Key Components**:

1. **Homogeneity Check**: Verify that for every edge triplet in the subgraph, both src_label and dst_label are members of the declared vertex label set.
2. **Error Messaging**: Provide clear error messages distinguishing between "heterogeneous subgraph" (invalid) and "multi-label homogeneous subgraph" (valid).

**Functional Requirements**:

1. **FR-010**: The system MUST accept subgraphs with multiple vertex labels if all edge triplet endpoints (src_label, dst_label) are within the declared vertex label set.
2. **FR-011**: The system MUST reject subgraphs where any edge triplet references a label not in the vertex label set, with a descriptive error message.
3. **FR-012**: The system MUST continue to reject subgraphs with zero vertex labels or zero edge triplets.

**Acceptance Scenarios**:

1. **Given** vertex_labels=[Person, TEMP_Person] and edge_triplets=[(Person,KNOWS,Person), (Person,KNOWS,TEMP_Person)], **When** validation runs, **Then** it passes (all endpoints in vertex set).
2. **Given** vertex_labels=[Person] and edge_triplets=[(Person,KNOWS,TEMP_Person)], **When** validation runs, **Then** it fails with error "TEMP_Person is not in the declared vertex label set".
3. **Given** vertex_labels=[Person] and edge_triplets=[(Person,KNOWS,Person)], **When** validation runs (single-label), **Then** it passes (backward compatible).

**Test Strategy**:

- **Unit Tests**: Test all combinations: single-label valid, multi-label valid (homogeneous), multi-label invalid (heterogeneous), empty labels.
- **Integration Tests**: Verify end-to-end that valid multi-label projections reach the algorithm and invalid ones produce user-friendly errors.

---

### Edge Cases

- What happens when multiple vertex labels have overlapping primary key values (e.g., both Person and TEMP_Person have a vertex with name="Alice")? → They are distinct vertices with distinct unified IDs; the view does not deduplicate by primary key.
- What happens when the user combines semantically unrelated labels (e.g., Person + Software)? → 系统不做语义合理性检查。只要结构合法（edge triplet 端点在 vertex label set 内）就允许执行。语义合理性由用户负责。
- What happens when one of the vertex labels has zero vertices (empty set)? → The view correctly handles it; the label contributes zero to the unified set but still occupies an offset range of size 0.
- What happens when edge triplets include self-loops across labels (e.g., TEMP_Person→KNOWS→TEMP_Person)? → Handled correctly as long as both endpoints are in the vertex label set.
- What happens when the algorithm produces results but some vertices have no edges? → Isolated vertices are still included in output (they belong to the unified vertex set).
- What happens with predicate-based filtering combined with multi-label projection? → 第一版不支持。多 label 投影下如果用户指定了 predicate，系统应拒绝并报错提示 "Predicates are not supported in multi-label projections"。后续迭代可考虑支持。

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: All existing GDS algorithms (Leiden, Louvain, PageRank, WCC, BFS, SSSP, CDLP, KCore, LCC) produce correct results on multi-label projected graphs without any per-algorithm code modifications.
- **SC-002**: On single-label projections, there is zero measurable performance regression compared to the current implementation.
- **SC-003**: Community detection (Leiden/Louvain) on a graph combining 10K persistent vertices + 5K temporary vertices produces community assignments covering all 15K vertices, with results equivalent to running on a single-label graph of identical topology.
- **SC-004**: The unified view construction adds no more than O(L) initialization overhead, where L is the number of labels (typically 2-5), independent of vertex/edge count.
- **SC-005**: Invalid multi-label projections (heterogeneous subgraphs) are rejected at query compilation time with a user-comprehensible error message.
