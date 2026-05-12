# Feature: Iceberg Data Lake Extension

**Input**: Design documents from `/specs/004-iceberg-extension/`
**Prerequisites**: plan.md (required), spec.md (required for modules)
**GitHub Feature Issue**: https://github.com/alibaba/neug/issues/343

# Modules

- Module 1: Iceberg Table Reading (P1)
    - [F004-T101] Iceberg Type Mapper
    - [F004-T102] Iceberg Metadata Parser
    - [F004-T103] Iceberg Manifest Parser
    - [F004-T104] Iceberg Snapshot Resolver
    - [F004-T105] Iceberg Format Sniffer
    - [F004-T106] Iceberg Options Builder
    - [F004-T107] Iceberg Read Function
    - [F004-T108] Delete File Handling
    - [F004-T109] Predicate Pushdown & Manifest Pruning
    - [F004-T110] Nested Type JSON Serialization

- Module 2: Storage Backend Integration (P2)
    - [F004-T201] VFS-Compatible Path Resolution
    - [F004-T202] S3/OSS Integration Verification

- Module 3: Iceberg Extension Lifecycle & Installation (P3)
    - [F004-T301] CMake Build Configuration
    - [F004-T302] Extension Entry Points & Registration
    - [F004-T303] Integration into Extension Build System

- Module 4: REST Catalog Integration (P4 — Future)
    - [F004-T401] REST Catalog Client
    - [F004-T402] Catalog Configuration Options Parsing
