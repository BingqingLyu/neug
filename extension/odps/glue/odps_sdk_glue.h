/** Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// ABI-neutral C seam between the outer `odps` extension (compiled with NeuG's
// default _GLIBCXX_USE_CXX11_ABI=1, C++20) and the inner glue library
// `odps_sdk_glue` (compiled with _GLIBCXX_USE_CXX11_ABI=0 to match the ODPS
// SDK, which hard-codes that ABI for GCC>=5).
//
// RULE: nothing in this header may expose a C++ STL type (std::string,
// std::vector, std::function, ...) or an SDK/Arrow type. Only POD structs,
// C strings (char*) and opaque handles cross the seam, so the two ABIs never
// share a std::string layout. All heap memory returned from the glue side is
// allocated with malloc/strdup and must be released with the matching
// odps_glue_*_free function (never with the caller's C++ delete/operator new).

#ifndef NEUG_EXTENSION_ODPS_GLUE_ODPS_SDK_GLUE_H_
#define NEUG_EXTENSION_ODPS_GLUE_ODPS_SDK_GLUE_H_

#include <stddef.h>

// The glue is built as a SHARED library with -fvisibility=hidden so that the
// SDK's ABI=0 Arrow/protobuf code it absorbs stays invisible to the process
// (see plan.md C3, RTLD_LOCAL isolation). Only the C seam below crosses the
// boundary, so each entry point is explicitly promoted back to default
// visibility; everything else in the .so remains hidden.
#if defined(_WIN32)
#define ODPS_GLUE_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define ODPS_GLUE_API __attribute__((visibility("default")))
#else
#define ODPS_GLUE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Connection configuration. Every field is a NUL-terminated C string that may
// be NULL or empty when the setting is not provided. The glue copies whatever
// it needs; the caller retains ownership of the pointed-to memory.
typedef struct OdpsGlueConfig {
  const char* access_id;
  const char* access_key;
  const char* endpoint;         // ODPS service endpoint (required)
  const char* tunnel_endpoint;  // optional; empty -> SDK default routing
  const char* project;          // optional connection-default project
  const char* quota_name;       // optional
  const char* region_id;        // optional
} OdpsGlueConfig;

// One data column of a table schema, as an ABI-neutral POD.
typedef struct OdpsGlueColumn {
  const char* name;  // owned by OdpsGlueSchema; valid until schema_free
  int type_code;     // numeric value of apsara::odps::sdk::ODPSColumnType
  int nullable;      // 0 or 1
} OdpsGlueColumn;

// Result of a schema sniff.
//  - success: columns/count filled, error == NULL;
//  - failure: columns == NULL, count == 0, error == heap message.
// Release with odps_glue_schema_free in both cases.
typedef struct OdpsGlueSchema {
  OdpsGlueColumn* columns;
  size_t count;
  char* error;
} OdpsGlueSchema;

// Opaque connection handle. Owns the SDK MaxStorageApi + IODPS core client.
typedef struct OdpsGlueConnection OdpsGlueConnection;

// Create and initialize a connection. Returns NULL on failure, in which case
// *out_error (when non-NULL) is set to a heap message the caller must free
// with odps_glue_free_string.
ODPS_GLUE_API OdpsGlueConnection* odps_glue_connect(
    const OdpsGlueConfig* config, char** out_error);

// Destroy a connection handle (safe on NULL).
ODPS_GLUE_API void odps_glue_disconnect(OdpsGlueConnection* conn);

// Sniff the data-column schema of `project`.`schema`.`table`.
// Returns 0 on success (out->columns/out->count filled); non-zero on failure
// (out->error set). Always release `out` with odps_glue_schema_free.
ODPS_GLUE_API int odps_glue_sniff_schema(OdpsGlueConnection* conn,
                                         const char* project,
                                         const char* schema, const char* table,
                                         OdpsGlueSchema* out);

// Release everything owned by an OdpsGlueSchema (columns, names, error).
ODPS_GLUE_API void odps_glue_schema_free(OdpsGlueSchema* schema);

// Free a heap string returned by the glue (e.g. an out_error message).
ODPS_GLUE_API void odps_glue_free_string(char* s);

// ---------------------------------------------------------------------------
// Data-plane reader (module 1, task T106).
//
// Iterates a Storage API TableReadSession's splits and exports each Arrow
// record batch across the ABI seam through the Arrow C Data Interface. Only
// POD structs, C strings, opaque handles and void* cross here; every
// SDK/Arrow C++ object stays on this ABI=0 side.
// ---------------------------------------------------------------------------

// Split/read knobs for opening a reader. Non-positive values mean "SDK
// default".
typedef struct OdpsGlueReadOptions {
  long long split_size_bytes;  // >0 -> SIZE split mode with this target size
  long long max_batch_rows;  // >0 -> ReadOptions.mMaxBatchRows (SDK max 20000)
  // ODPS filter-predicate string in the SDK's IPredicate::ToString() /
  // SetFilterPredicate dialect (see include/odps_predicate_converter.h). NULL
  // or empty -> no predicate pushdown. Non-empty -> the glue wraps it as an
  // IRawPredicate on FilterOptions.mPredicate so the read session filters rows
  // server-side (module 3, task T303). Callers still re-apply the full filter
  // after decoding, so this only reduces how many rows are transferred.
  const char* filter_predicate;
  // Partition specs to prune to (module 3, task T302). Each entry is one
  // complete partition path in the SDK's '/'-delimited dialect (e.g.
  // "pt=1/ds=x") and maps 1:1 onto FilterOptions.mRequiredPartitions, so the
  // session reads exactly these partitions. NULL or a zero count -> read all
  // partitions. The glue copies the strings it needs; the caller retains
  // ownership of the array and every element.
  const char* const* required_partitions;
  size_t required_partition_count;
} OdpsGlueReadOptions;

// Opaque reader handle. Owns a TableReadSession, its split list, the split
// cursor and the currently-open TableReadStream. The OdpsGlueConnection it was
// opened from must outlive it.
typedef struct OdpsGlueReader OdpsGlueReader;

// Open a read session over `project`.`schema`.`table`. Returns NULL on failure,
// in which case *out_error (when non-NULL) is set to a heap message the caller
// must free with odps_glue_free_string.
ODPS_GLUE_API OdpsGlueReader* odps_glue_open_reader(
    OdpsGlueConnection* conn, const char* project, const char* schema,
    const char* table, const OdpsGlueReadOptions* options, char** out_error);

// Number of splits in the session (>=0), or -1 on error (*out_error set).
ODPS_GLUE_API long long odps_glue_reader_split_count(OdpsGlueReader* reader,
                                                     char** out_error);

// Total record count across all splits (>=0), or -1 on error/unknown.
ODPS_GLUE_API long long odps_glue_reader_record_count(OdpsGlueReader* reader,
                                                      char** out_error);

// Fetch the next record batch, exported through the Arrow C Data Interface.
// `out_array`/`out_schema` point to caller-owned structs with the canonical
// ArrowArray/ArrowSchema layout; the glue fills them (including the release
// callbacks), and the caller MUST invoke those release callbacks when done.
// Returns:
//   1  -> a batch was produced (out_array/out_schema filled);
//   0  -> end of data (all splits exhausted; structs left untouched);
//  <0  -> error (*out_error set).
ODPS_GLUE_API int odps_glue_reader_next_batch(OdpsGlueReader* reader,
                                              void* out_array, void* out_schema,
                                              char** out_error);

// Close and destroy a reader (safe on NULL); closes any open stream first.
ODPS_GLUE_API void odps_glue_reader_close(OdpsGlueReader* reader);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // NEUG_EXTENSION_ODPS_GLUE_ODPS_SDK_GLUE_H_
