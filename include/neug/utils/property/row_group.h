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

#include <cstdint>

#include "neug/storages/chunk/row_group.h"
#include "neug/utils/exception/exception.h"
#include "neug/utils/property/table.h"

namespace neug {

/// Table-level logical row group view (layout B). A RowGroup references a
/// contiguous row range [begin(), end()) of a table and logically "contains"
/// every column over that range. It owns no data: the columns stay flat in the
/// Table and are read by global vid = begin() + offset. This is the row-level
/// counterpart of ChunkedColumn (which splits one column); a RowGroup splits
/// the whole row set. See specs/012-row-group-view.
///
/// The view is read-only; writes still go through the existing column paths.
/// The frontier()/row_count() abstraction reserves the semantics for
/// group-internal compaction (frontier retreats, inserts refill the group tail)
/// delivered by a later spec; no physical per-group allocation unit is
/// introduced here.
struct RowGroup {
  const Table* table = nullptr;
  RowGroupLayout layout;
  uint64_t group_id = 0;

  /// First global vid of the group (base_g = group_id * G).
  uint64_t begin() const { return layout.GroupBegin(group_id); }
  /// One-past-last global vid, clamped to the table size (tail group).
  uint64_t end() const { return layout.GroupEnd(group_id, table->size()); }
  /// Number of row slots in the group.
  uint64_t row_count() const { return end() - begin(); }
  /// Global vid of the next append slot. In the initial dense state this equals
  /// end(); after group-internal compaction it retreats to begin()+valid_count.
  uint64_t frontier() const { return end(); }

  /// The col_id-th column of the table, viewed over this group's row range.
  const ColumnBase* column(size_t col_id) const {
    return table->get_column_by_id(col_id);
  }
  /// Read a value by group-local offset; resolves to global vid begin()+offset.
  Value get(size_t col_id, uint64_t offset) const {
    if (offset >= row_count())
      THROW_RUNTIME_ERROR("Row group offset out of range");
    return table->get_column_by_id(col_id)->get_any(begin() + offset);
  }
};

}  // namespace neug