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
#pragma once

#include <algorithm>
#include <cstdint>

#include "neug/storages/chunk/chunk_types.h"
#include "neug/utils/exception/exception.h"

namespace neug {

/// Pure-arithmetic row group layout descriptor (layout B logical overlay).
///
/// A row group spans a fixed, uniform row range [g*G, (g+1)*G) shared by every
/// column of a table. The group boundary is a vid arithmetic boundary, not a
/// physical allocation boundary: columns keep their flat pages_ array indexed
/// by global vid, and no per-group directory is maintained. This struct only
/// carries the group size and the arithmetic; it owns no data.
///
/// The vid address space stays contiguous and uniform (base_g = g*G), while
/// valid rows need not be globally dense: group-internal compaction gathers
/// valid rows to the front and leaves holes at the group tail.
struct RowGroupLayout {
  uint64_t rows_per_group;

  explicit RowGroupLayout(uint64_t rows_per_group = kDefaultRowsPerGroup)
      : rows_per_group(rows_per_group) {}

  /// Group owning a global vid.
  uint64_t GroupOf(uint64_t vid) const { return vid / rows_per_group; }
  /// Offset of a global vid within its group.
  uint64_t OffsetOf(uint64_t vid) const { return vid % rows_per_group; }
  /// First global vid of a group (base_g = g*G).
  uint64_t GroupBegin(uint64_t group_id) const {
    return group_id * rows_per_group;
  }
  /// One-past-last global vid of a group, clamped to the table size (tail
  /// group).
  uint64_t GroupEnd(uint64_t group_id, uint64_t table_size) const {
    return std::min((group_id + 1) * rows_per_group, table_size);
  }
  /// Number of groups covering table_size rows (ceil division).
  uint64_t GroupCount(uint64_t table_size) const {
    return (table_size + rows_per_group - 1) / rows_per_group;
  }

  /// A group must be a whole number of logical chunks: rows_per_group is a
  /// power of two, at least rows_per_chunk, and divisible by it. Throws
  /// otherwise.
  static void Validate(uint64_t rows_per_group, uint64_t rows_per_chunk) {
    if (rows_per_group == 0 || (rows_per_group & (rows_per_group - 1)) != 0 ||
        rows_per_group < rows_per_chunk || rows_per_group % rows_per_chunk != 0)
      THROW_INVALID_ARGUMENT_EXCEPTION("Invalid rows_per_group");
  }
};

}  // namespace neug