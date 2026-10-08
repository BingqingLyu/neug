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

// Vendored copy of the Arrow C Data Interface structs (ArrowSchema/ArrowArray),
// which are a *stable, version-agnostic C ABI* standardized by Apache Arrow.
// The struct layout below is byte-for-byte the canonical definition from
// Arrow's `arrow/c/abi.h` (Apache-2.0), renamed with an `Odps` prefix so this
// header can never collide with a real Arrow header pulled in elsewhere.
//
// Why vendor instead of including <arrow/c/abi.h>?  The ODPS SDK's Arrow (and
// thus abi.h) only exists on the ABI=0 glue side of the seam. The outer
// extension is Arrow-free by design (NeuG core keeps zero Arrow). The glue
// exports each record batch through these structs (arrow::ExportRecordBatch);
// the outer layer consumes them here and copies the data into owned NeuG
// columns. Because the layout is standardized, the void* pointers cross the
// ABI seam safely and the producer-set `release` callback (an Arrow function
// pointer) can be invoked from this side unchanged.
//
// Ownership: the *producer* (glue) fills these structs and installs `release`;
// the *consumer* (outer supplier) MUST call `release(&struct)` exactly once
// when it is done, which frees producer-owned memory and nulls `release`.

#ifndef NEUG_EXTENSION_ODPS_INCLUDE_ODPS_ARROW_ABI_H_
#define NEUG_EXTENSION_ODPS_INCLUDE_ODPS_ARROW_ABI_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ODPS_ARROW_FLAG_DICTIONARY_ORDERED 1
#define ODPS_ARROW_FLAG_NULLABLE 2
#define ODPS_ARROW_FLAG_MAP_KEYS_SORTED 4

struct OdpsArrowSchema {
  // Array type description
  const char* format;
  const char* name;
  const char* metadata;
  int64_t flags;
  int64_t n_children;
  struct OdpsArrowSchema** children;
  struct OdpsArrowSchema* dictionary;

  // Release callback
  void (*release)(struct OdpsArrowSchema*);
  // Opaque producer-specific data
  void* private_data;
};

struct OdpsArrowArray {
  // Array data description
  int64_t length;
  int64_t null_count;
  int64_t offset;
  int64_t n_buffers;
  int64_t n_children;
  const void** buffers;
  struct OdpsArrowArray** children;
  struct OdpsArrowArray* dictionary;

  // Release callback
  void (*release)(struct OdpsArrowArray*);
  // Opaque producer-specific data
  void* private_data;
};

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // NEUG_EXTENSION_ODPS_INCLUDE_ODPS_ARROW_ABI_H_
