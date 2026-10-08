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

#include <memory>

#include "neug/common/types/data_chunk.h"
#include "odps_arrow_abi.h"

namespace neug {
namespace extension {
namespace odps {

/**
 * @brief Converts an exported Arrow record batch into an owned NeuG DataChunk.
 *
 * This is the ABI=1 side of the Arrow C Data Interface bridge (plan.md C3,
 * algorithm 4). The inner ABI=0 glue reads `arrow::RecordBatch` from the ODPS
 * Storage API and exports it as a flat top-level struct via
 * `arrow::ExportRecordBatch`, filling an `OdpsArrowSchema`/`OdpsArrowArray`
 * pair; this function walks those C structs and copies every value into owned
 * NeuG value columns. It has no Arrow C++ / SDK dependency, so it compiles and
 * is unit-tested even in a `NEUG_WITH_ODPS_SDK=OFF` build.
 *
 * The returned DataChunk never retains pointers into the ArrowArray buffers:
 * all data is deep-copied, so the caller may (and must) release the C Data
 * Interface structs immediately after this returns.
 *
 * v1 covers flat structs of scalar columns (integer widths, float, double,
 * boolean, string, plus date/timestamp for forward-compatibility). Dictionary,
 * decimal and nested (list/struct/map) columns raise a located
 * `THROW_INVALID_ARGUMENT_EXCEPTION` and are deferred to module 4 (T401).
 *
 * @param schema the exported struct schema (root format must be "+s").
 * @param array  the exported struct array (one child per column).
 * @return an owned DataChunk with one NeuG column per Arrow struct field.
 */
std::shared_ptr<DataChunk> recordBatchToDataChunk(const OdpsArrowSchema& schema,
                                                  const OdpsArrowArray& array);

}  // namespace odps
}  // namespace extension
}  // namespace neug
