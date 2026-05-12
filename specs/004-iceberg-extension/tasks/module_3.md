# Module 3: Iceberg Extension Lifecycle & Installation (P3)

**Goal**: Provide the standard NeuG extension lifecycle (INSTALL, LOAD, Init, Name) for the Iceberg extension, following the same pattern as existing extensions (JSON, Parquet, S3) to ensure consistent user experience.

**Assignee**: 
**Label**: iceberg
**Milestone**: 
**Project**: 

## [F004-T301] CMake Build Configuration

**description**: Create the CMakeLists.txt for the Iceberg extension that builds it as a dynamically-loaded shared library (`.neug_extension`), with correct dependency linking.

**details**:
* Create `extension/iceberg/CMakeLists.txt` following the Parquet extension pattern:
  - `file(GLOB_RECURSE ICEBERG_SRC src/*.cpp)`
  - `file(GLOB_RECURSE ICEBERG_HEADERS include/*.h)`
  - Call `build_extension_lib("iceberg")` which creates the shared library target
  - Set include directories: `target_include_directories(neug_iceberg_extension PRIVATE include/)`
* Link dependencies:
  - `neug` (core library)
  - `ARROW_BASE_LIB` (Arrow core)
  - `arrow_dataset_objlib` or `arrow_dataset_shared` (for DatasetFactory, Scanner)
  - `parquet_objlib` or `parquet_shared` (for reading Parquet data files)
  - RapidJSON (header-only, from `third_party/rapidjson/`)
* Avro parsing dependency decision:
  - Option A: Use Arrow's built-in Avro support if available
  - Option B: Read manifest list/files as Parquet (some Iceberg implementations support this)
  - Option C: Bundle a lightweight Avro C++ library
* Create `extension/iceberg/tests/CMakeLists.txt`:
  - Use `add_extension_test()` macro for C++ unit tests
  - Link test targets against gtest and the iceberg extension

## [F004-T302] Extension Entry Points & Registration

**description**: Implement the standard extension entry points (`Init()`, `Name()`) and function registration following the NeuG extension contract.

**details**:
* Create `extension/iceberg/include/iceberg_extension.h` and `extension/iceberg/src/iceberg_extension.cpp`
* Implement `extern "C"` entry points:
  ```cpp
  extern "C" {
      void Init(neug::main::ClientContext* context) {
          ExtensionAPI::registerFunction<IcebergReadFunction>(TABLE_FUNCTION_ENTRY);
          ExtensionAPI::registerExtension(ExtensionInfo{"iceberg", "Apache Iceberg data lake table reader"});
      }
      const char* Name() { return "ICEBERG"; }
  }
  ```
* The `Init()` function must:
  - Register `IcebergReadFunction` as a `TABLE_FUNCTION_ENTRY`
  - Register extension metadata with name `"iceberg"` and a descriptive string
  - Register the Iceberg sniffer for auto-detection (if sniffer registration API is available)
* `Name()` must return `"ICEBERG"` (uppercase, matches INSTALL/LOAD command syntax)
* Verify the extension is correctly discoverable via `CALL show_extensions()` after loading

## [F004-T303] Integration into Extension Build System

**description**: Add the Iceberg extension to the top-level extension build system so it is compiled as part of the NeuG build when enabled.

**details**:
* Modify `extension/CMakeLists.txt`:
  - Add `add_extension_if_enabled("iceberg")` entry alongside parquet, json, s3
* Verify that the extension builds correctly:
  - `mkdir build && cd build && cmake .. -DBUILD_EXTENSIONS="iceberg" && make -j$(nproc)`
  - Confirm output: `build/extension/iceberg/neug_iceberg_extension.neug_extension`
* Verify extension properties set by `set_extension_properties()`:
  - Output suffix: `.neug_extension`
  - RPATH configured for finding `libneug` at runtime
  - Install target places extension in correct directory
* Add the iceberg extension to the default extension list (or keep it opt-in based on project policy)
* Ensure `make format-check` passes on all new files (clang-format compliance)
