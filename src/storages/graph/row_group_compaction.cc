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

#include "neug/storages/graph/row_group_compaction.h"

#include <limits>

#include "neug/storages/graph/vertex_table.h"

namespace neug {

std::vector<vid_t> GenerateGroupReorderMap(const VertexTable& table,
                                           size_t group_id, timestamp_t ts) {
  const RowGroupLayout layout = table.row_group_layout();
  const size_t begin = layout.GroupBegin(group_id);
  const size_t end = layout.GroupEnd(group_id, table.LidNum());
  const size_t capacity = end - begin;
  const vid_t invalid = std::numeric_limits<vid_t>::max();
  std::vector<vid_t> mapping(capacity, invalid);
  const VertexTimestamp& v_ts = table.get_vertex_timestamp();
  size_t next_offset = 0;
  for (size_t offset = 0; offset < capacity; ++offset) {
    const vid_t old_vid = static_cast<vid_t>(begin + offset);
    if (v_ts.IsVertexValid(old_vid, ts)) {
      mapping[offset] = static_cast<vid_t>(begin + next_offset);
      ++next_offset;
    }
  }
  return mapping;
}

}  // namespace neug