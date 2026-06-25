# Feature Specification: COPY TEMP from Cypher Query Results

**Feature Branch**: `005-copy-temp-query`  
**Created**: 2026-06-25  
**Status**: Draft  
**Input**: Extend COPY TEMP to support Cypher query results as data source, enabling materialization of intermediate graph computation results into temporary node/edge tables for further querying.

## Background & Motivation

Current `COPY TEMP` only supports external file sources (CSV, JSON, JSONL, Parquet). In graph analysis workflows, users frequently compute intermediate results (e.g., filtered subgraphs, centrality metrics, community labels) that they want to temporarily persist for further querying — without polluting the persistent database and without exporting to an intermediate file.

This extension allows `COPY TEMP ... FROM (Cypher query)` where the subquery is a full Cypher `MATCH ... RETURN` statement, reusing all existing temporary graph infrastructure (schema marking, lifecycle management, mixed queries).

## Syntax

```sql
-- Materialize node set from query results
COPY TEMP TempHighDegree FROM (
    MATCH (p:Person)-[:KNOWS]->(f)
    WITH p, count(f) AS degree
    WHERE degree > 10
    RETURN p.id AS id, p.name AS name, degree
);

-- Materialize edge set from query results
COPY TEMP TempRecommend FROM (
    MATCH (a:Person)-[:KNOWS]->()-[:KNOWS]->(b:Person)
    WHERE a <> b AND NOT (a)-[:KNOWS]->(b)
    RETURN DISTINCT a.id AS src_id, b.id AS dst_id
) (from='Person', to='Person');
```

**Key semantics**:
- No `from`/`to` options → node table; first column is primary key (or `primary_key` option specifies which column)
- With `from`/`to` options → edge table; first two columns are src/dst keys
- The subquery is a **read-only** Cypher query that produces tabular output
- Results are **fully materialized** before insertion (no streaming read-write on same graph)

## Structural Constraint

Temporary graph tables must conform to graph topology:
- **Node table**: requires at least one column as primary key (unique identifier per vertex)
- **Edge table**: requires at least two columns as src/dst (referencing existing vertex labels)

This means not all query results can be materialized — results must represent graph elements (node sets or edge sets). Pure aggregate/statistical results (e.g., `RETURN count(*)`) are not valid sources.

## Functional Modules *(mandatory)*

### Module 1: Cypher Subquery Parsing & Binding (Priority: P1)

**Purpose**: Enable the parser and binder to accept a full Cypher query (MATCH/WITH/WHERE/RETURN) as the subquery source inside `COPY TEMP ... FROM (...)`, replacing or extending the current `LOAD FROM` subquery path.

**Why this priority**: Without this, no other module can function. This is the entry point for the entire feature.

**Independent Test**: Can be tested by verifying that `COPY TEMP TempX FROM (MATCH ... RETURN ...)` parses and binds correctly, producing a valid logical plan with the correct output schema.

**Key Components**:

1. **Grammar Extension**: Extend the `COPY TEMP ... FROM (subquery)` grammar rule to accept Cypher read-query statements (MATCH/OPTIONAL MATCH/WITH/WHERE/RETURN) in addition to LOAD FROM.
2. **Binder Dispatch**: Detect whether the subquery is a `LOAD FROM` (file source) or a Cypher query (graph source), and route to the appropriate binding logic.
3. **Schema Inference**: Infer the output schema (column names + types) from the RETURN clause's bound expressions at compile time.

**Functional Requirements**:

1. **FR-001**: System MUST parse `COPY TEMP <label> FROM (<cypher-read-query>)` without syntax errors when the inner query is a valid read-only Cypher statement.
2. **FR-002**: System MUST reject inner queries that contain write operations (INSERT, DELETE, SET, REMOVE, MERGE) with a clear error message.
3. **FR-003**: System MUST infer the temporary table's schema from the RETURN clause, including column names and data types.
4. **FR-004**: System MUST validate that the RETURN produces at least 1 column for node tables, or at least 2 columns for edge tables.
5. **FR-005**: System MUST validate that `from`/`to` referenced labels exist (persistent or temporary) at bind time.

**Acceptance Scenarios**:

1. **Given** a persistent graph with Person nodes, **When** user executes `COPY TEMP TempTop FROM (MATCH (p:Person) RETURN p.id AS id, p.name AS name)`, **Then** the statement parses, binds, and produces a plan with output schema `{id: INT64, name: STRING}`.
2. **Given** a Cypher query with INSERT inside the subquery, **When** user executes `COPY TEMP TempX FROM (MATCH (p:Person) INSERT (:Foo {id: p.id}) RETURN p.id)`, **Then** the system returns a binder error indicating write operations are not allowed in COPY TEMP source queries.
3. **Given** a COPY TEMP with `from`/`to` options, **When** the RETURN clause produces only 1 column, **Then** the system returns a binder error indicating edge tables require at least 2 columns.

**Test Strategy**:

- **Unit Tests**: Parser tests for valid/invalid grammar; Binder tests for schema inference, write-rejection, and column count validation.
- **Integration Tests**: Full compile pipeline producing a physical plan for a simple COPY TEMP from query.

---

### Module 2: Execution with Materialization Barrier (Priority: P1)

**Purpose**: Execute the Cypher subquery, fully collect its results into a materialized intermediate representation, then insert the results into the temporary table — ensuring no concurrent read-write conflicts on the same graph.

**Why this priority**: Core execution semantics. Without materialization, the system could read and write the same graph simultaneously, leading to correctness issues.

**Independent Test**: Can be tested by executing a COPY TEMP from a simple query and verifying the temporary table contains the expected rows.

**Key Components**:

1. **Query Execution**: Execute the inner Cypher query as a standard read query against the current transaction snapshot.
2. **Result Materialization**: Collect all output rows into memory before beginning insertion into the temporary table.
3. **Temporary Table Insertion**: Reuse existing COPY TEMP insertion logic (schema creation + bulk insert) with the materialized rows as input.

**Functional Requirements**:

1. **FR-006**: System MUST execute the inner query to completion and collect all results before creating the temporary table.
2. **FR-007**: System MUST use the current transaction's read snapshot for the inner query, ensuring a consistent view of the graph.
3. **FR-008**: System MUST report an error if materialized results violate primary key uniqueness (for node tables).
4. **FR-009**: System MUST silently skip rows where src/dst references do not exist in the target vertex table (for edge tables), consistent with existing COPY TEMP behavior.
5. **FR-010**: System MUST support all data types that are valid in RETURN expressions as temporary table property types.

**Acceptance Scenarios**:

1. **Given** a graph with 100 Person nodes, **When** user executes `COPY TEMP TempAll FROM (MATCH (p:Person) RETURN p.id AS id, p.name AS name)`, **Then** TempAll contains exactly 100 rows with correct id and name values.
2. **Given** a query that produces duplicate primary keys, **When** user executes the COPY TEMP, **Then** the system returns an error about duplicate primary key.
3. **Given** a query that returns edges referencing non-existent source vertices, **When** user executes the COPY TEMP with `from`/`to`, **Then** those rows are silently skipped.

**Test Strategy**:

- **Unit Tests**: Materialization correctness, duplicate PK detection, dangling reference handling.
- **Integration Tests**: End-to-end execution from query to queryable temporary table.

---

### Module 3: Mixed Query Support (Priority: P2)

**Purpose**: After materialization, the temporary table participates in standard Cypher queries alongside persistent and other temporary tables — enabling multi-hop analysis workflows.

**Why this priority**: This is the primary use case motivation — users materialize intermediate results to build upon them in subsequent queries.

**Independent Test**: Can be tested by materializing a temporary table from a query, then running a subsequent MATCH that joins the temporary table with persistent data.

**Key Components**:

1. **Query Chain**: Users execute multiple COPY TEMP statements sequentially, each building on previous results.
2. **Cross-table MATCH**: Standard MATCH patterns reference both persistent labels and materialized temporary labels.

**Functional Requirements**:

1. **FR-011**: System MUST allow MATCH patterns that reference labels created by COPY TEMP from query sources, identical to COPY TEMP from file sources.
2. **FR-012**: System MUST allow chained COPY TEMP operations where a later COPY TEMP references temporary tables created by earlier COPY TEMP operations (both file-sourced and query-sourced).
3. **FR-013**: System MUST allow the inner Cypher query of COPY TEMP to reference existing temporary tables (created by prior COPY TEMP operations in the same session).

**Acceptance Scenarios**:

1. **Given** a materialized TempHighDegree node table, **When** user executes `MATCH (p:TempHighDegree)-[:KNOWS]->(f:Person) RETURN p.name, f.name`, **Then** the query returns correct results joining temporary and persistent data.
2. **Given** TempA created from a file and TempB created from a query referencing TempA, **When** user queries both, **Then** results reflect the correct chain of derivations.

**Test Strategy**:

- **Unit Tests**: Query planning with mixed temporary/persistent labels.
- **Integration Tests**: Multi-step analysis workflow (materialize → query → materialize → query).

---

### Module 4: Lifecycle & Error Handling (Priority: P2)

**Purpose**: Ensure materialized temporary tables follow the same lifecycle rules as file-sourced temporary tables, and provide clear error messages for constraint violations.

**Why this priority**: Consistency with existing COPY TEMP behavior is essential for user expectations.

**Independent Test**: Can be tested by verifying that materialized tables are cleaned up on connection close, and that DROP TABLE works.

**Key Components**:

1. **Lifecycle Parity**: Same cleanup behavior as file-sourced COPY TEMP (auto-cleanup on Connection close, explicit DROP TABLE).
2. **Error Reporting**: Clear messages for constraint violations (write in subquery, insufficient columns, type mismatch, duplicate pk).

**Functional Requirements**:

1. **FR-014**: System MUST automatically drop query-sourced temporary tables when the Connection closes, identical to file-sourced temporary tables.
2. **FR-015**: System MUST support `DROP TABLE <label>` for explicit release of query-sourced temporary tables.
3. **FR-016**: System MUST reject COPY TEMP from query in read-only mode with the same error message as file-sourced COPY TEMP.
4. **FR-017**: System MUST reject COPY TEMP from query if the target label already exists (same conflict rules as file-sourced COPY TEMP).

**Acceptance Scenarios**:

1. **Given** a materialized TempX, **When** the Connection is closed and reopened, **Then** TempX no longer exists.
2. **Given** a materialized TempX, **When** user executes `DROP TABLE TempX`, **Then** TempX is removed and subsequent queries cannot reference it.
3. **Given** a read-only connection, **When** user attempts COPY TEMP from query, **Then** the system returns an error indicating write operations are not allowed in read-only mode.

**Test Strategy**:

- **Unit Tests**: Lifecycle cleanup verification, error message content.
- **Integration Tests**: Full workflow with explicit DROP and connection close scenarios.

---

### Edge Cases

- What happens when the inner query returns zero rows? → Empty temporary table is created (valid, queryable, returns no results).
- What happens when the inner query references the same label being created? → Not possible — the label doesn't exist yet at query execution time.
- What happens when the inner query is very large (millions of rows)? → Memory pressure from materialization; future optimization could add spill-to-disk, but initial implementation is in-memory only.
- What happens with NULL values in primary key columns? → Error: primary key must not be NULL.
- What happens when RETURN uses expressions without aliases? → Binder infers column names from expression structure (existing behavior for RETURN).

## Assumptions

1. The inner Cypher query is a **single read statement** (not a multi-statement transaction).
2. Initial implementation materializes all results **in memory**. Spill-to-disk is a future optimization if needed.
3. The existing `COPY TEMP` insertion pipeline (schema creation, property column allocation) is reused without modification.
4. Data type support matches what the existing temporary table storage supports (INT64, DOUBLE, STRING, BOOL, DATE, etc.).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can materialize Cypher query results into temporary node tables and immediately query them in subsequent MATCH statements.
- **SC-002**: Users can materialize Cypher query results into temporary edge tables (with `from`/`to`) that connect to existing persistent or temporary vertex tables.
- **SC-003**: Chained materialization (COPY TEMP A → query A → COPY TEMP B → query B) works correctly for multi-step analysis workflows.
- **SC-004**: All constraint violations (write in subquery, insufficient columns, duplicate pk, type mismatch) produce clear, actionable error messages.
- **SC-005**: Query-sourced temporary tables have identical lifecycle behavior to file-sourced temporary tables (connection-scoped, DROP-able, checkpoint-invisible).
- **SC-006**: Materialization of 100K rows completes within 5 seconds on standard hardware.
