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
#include <memory>
#include <string>
#include <vector>

#include "neug/common/types/data_chunk.h"
#include "neug/storages/loader/loader_utils.h"
#include "neug/utils/io/read/common/read_state.h"
#include "odps_connection.h"

namespace neug {
namespace extension {
namespace odps {

/**
 * @brief Streams an ODPS/MaxCompute table as owned NeuG DataChunks.
 *
 * This is the lazy (batch_read) side of `ODPS_SCAN` and the workhorse behind
 * the eager `execFunc` too. On construction it resolves the source table and
 * credentials from the `ReadSharedState`, opens a Storage API read session
 * through the inner ABI=0 glue (glue/odps_sdk_glue.h), and caches the session
 * row count. Each `GetNextChunk()` pulls one Arrow record batch across the C
 * seam, converts it with `recordBatchToDataChunk` (the SDK-free ABI=1 bridge),
 * immediately releases the exported C Data Interface structs, and hands back an
 * owned chunk. `nullptr` signals end-of-data (every split exhausted); a real
 * read error raises a located `THROW_IO_EXCEPTION`.
 *
 * The supplier owns both the connection and the opaque reader handle and closes
 * the reader before the connection unwinds, so the reader never outlives the
 * SDK objects it was built from. This header is SDK-free: the reader is stored
 * as an opaque `void*` and every glue call is confined to the .cc under the
 * `ODPS_SDK_ENABLE_ARROW` gate. In a `NEUG_WITH_ODPS_SDK=OFF` build the
 * constructor throws a clear "requires the ODPS SDK build" error.
 */
class OdpsRecordBatchSupplier : public IDataChunkSupplier {
 public:
  // `enableColumnPruning` turns on T301 column pruning (request only the
  // projected + filtered columns). It is ON for the eager `LOAD FROM ...
  // RETURN` path (execFunc) and OFF for the lazy path (supplierFunc, used by
  // COPY ... FROM (LOAD FROM ...)), which maps source columns BY INDEX against
  // the full table schema and must therefore keep reading every column.
  explicit OdpsRecordBatchSupplier(
      std::shared_ptr<reader::ReadSharedState> state,
      bool enableColumnPruning = true);
  ~OdpsRecordBatchSupplier() override;

  OdpsRecordBatchSupplier(const OdpsRecordBatchSupplier&) = delete;
  OdpsRecordBatchSupplier& operator=(const OdpsRecordBatchSupplier&) = delete;

  std::shared_ptr<DataChunk> GetNextChunk() override;

  // Total rows across all splits, or -1 when the session reports no count.
  int64_t RowNum() const override { return row_num_; }

  // Ordered names of the columns each chunk from GetNextChunk() contains
  // (module 3, T301). This is the pruned projection -- the queried output
  // columns plus every column the engine-side filter references, in
  // table-schema order -- and matches the layout of the returned Arrow batches.
  // execFunc decodes chunks against this list (not the full schema) so
  // filter_chunk / project_chunk stay positionally correct after column
  // pruning. Empty when no schema was available (no pruning info).
  const std::vector<std::string>& PhysicalColumnNames() const {
    return physical_column_names_;
  }

 private:
  // Keeps the SDK connection alive for as long as the reader depends on it.
  std::unique_ptr<OdpsConnection> connection_;
#if defined(ODPS_SDK_ENABLE_ARROW)
  // Opaque `OdpsGlueReader*` owned by the inner ABI=0 glue. Present only in an
  // SDK build; without the SDK the constructor throws before a reader exists,
  // so the member is gated out to keep the skeleton build warning-free.
  void* reader_ = nullptr;
#endif
  int64_t row_num_ = 0;
  // Column names the produced chunks carry, in schema order (see
  // PhysicalColumnNames). SDK-free, so it is present in both builds.
  std::vector<std::string> physical_column_names_;
};

}  // namespace odps
}  // namespace extension
}  // namespace neug
