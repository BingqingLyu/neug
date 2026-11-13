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
#include <vector>

#include "neug/utils/api.h"
#include "neug/utils/property/types.h"

namespace neug {

class VertexTable;

/// Logical compaction (spec 013): group-internal vid reorder.
///
/// Generates the old_vid -> new_vid compact mapping for one row group: valid
/// rows are gathered to the front of the group (new_vid = base_g + compact
/// offset), deleted rows get no mapping (kInvalidVid) and are dropped so their
/// bytes are reclaimed via GC. base_g is unchanged; clean groups are untouched.
///
/// The returned vector is indexed by group-local offset [0, group_capacity):
/// entry = new global vid for a valid row, or kInvalidVid for a deleted/absent
/// row. The mapping only covers valid vids; it is a transient reorder artifact
/// (not persisted) consumed by the four-structure remap.
NEUG_API std::vector<vid_t> GenerateGroupReorderMap(
    const VertexTable& table, size_t group_id, timestamp_t ts = MAX_TIMESTAMP);

}  // namespace neug