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

#include "odps_record_batch_supplier.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "neug/utils/exception/exception.h"
#include "odps_arrow_abi.h"
#include "odps_arrow_bridge.h"
#include "odps_error.h"
#include "odps_options.h"
#include "odps_predicate_converter.h"

#if defined(ODPS_SDK_ENABLE_ARROW)
#include "odps_sdk_glue.h"
#endif

namespace neug {
namespace extension {
namespace odps {

#if defined(ODPS_SDK_ENABLE_ARROW)
namespace {

// Releases the Arrow C Data Interface structs the glue installed (including
// the producer-set `release` callbacks) exactly once, on every exit path —
// including an exception thrown by the converter. `recordBatchToDataChunk`
// deep-copies, so releasing right after conversion is always safe.
struct ArrowCReleaseGuard {
  OdpsArrowArray* array;
  OdpsArrowSchema* schema;
  ~ArrowCReleaseGuard() {
    if (array != nullptr && array->release != nullptr) {
      array->release(array);
    }
    if (schema != nullptr && schema->release != nullptr) {
      schema->release(schema);
    }
  }
};

}  // namespace

OdpsRecordBatchSupplier::OdpsRecordBatchSupplier(
    std::shared_ptr<reader::ReadSharedState> state) {
  if (!state) {
    THROW_INVALID_ARGUMENT_EXCEPTION("ODPS_SCAN: null read state");
  }
  const reader::FileSchema& file = state->schema.file;
  OdpsSourceDesc source = OdpsOptions::fromFileSchema(file);
  if (source.table.empty()) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "ODPS_SCAN: cannot read without a table name");
  }

  auto connectionOptions = OdpsConnectionOptionsBuilder(file).build();
  connection_ = std::make_unique<OdpsConnection>(connectionOptions);

  // The address may omit the project; fall back to the connection default.
  const std::string project =
      source.hasProject() ? source.project : connection_->options().project;
  if (project.empty()) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "ODPS_SCAN: cannot read \"" + source.table +
        "\": no project given in the odps:// address and no default project on "
        "the connection (set the `project` option or ODPS_PROJECT)");
  }

  auto* glue = static_cast<OdpsGlueConnection*>(connection_->glueHandle());
  if (glue == nullptr) {
    THROW_RUNTIME_ERROR(
        "ODPS_SCAN: ODPS connection is unavailable on this handle");
  }

  // Split/read knobs: a positive split_size_mb hint switches the session to
  // SIZE split mode; batch row count is left at the SDK default for v1
  // (module 3 tunes batching/pushdown further).
  OdpsGlueReadOptions readOptions;
  readOptions.split_size_bytes =
      source.hasSplitSize()
          ? static_cast<long long>(source.splitSizeMb) * 1024 * 1024
          : 0;
  readOptions.max_batch_rows = 0;

  // Predicate pushdown (module 3, T303): translate the engine's skip_rows
  // filter into the ODPS predicate string dialect so the read session can
  // filter rows server-side. This is a pure transfer-reduction optimization --
  // execFunc still re-applies the full predicate via reader::filter_chunk -- so
  // an unsupported expression pushes nothing (fullyPushed=false) and stays
  // correct. `conversion` must outlive the open_reader call below, which copies
  // the string into the SDK predicate; as a ctor local, it does.
  OdpsPredicateConversion conversion;
  if (state->skipRows) {
    conversion = OdpsPredicateConverter::convert(*state->skipRows);
  }
  readOptions.filter_predicate =
      conversion.fullyPushed ? conversion.predicate.c_str() : nullptr;

  char* error = nullptr;
  OdpsGlueReader* reader =
      odps_glue_open_reader(glue, project.c_str(), source.schema.c_str(),
                            source.table.c_str(), &readOptions, &error);
  if (reader == nullptr) {
    const std::string message = (error != nullptr) ? error : "unknown error";
    odps_glue_free_string(error);
    OdpsError::throwAttributed(
        "ODPS_SCAN: failed to open a read session on " + project + "." +
            source.schema + "." + source.table,
        message,
        {connection_->options().accessId, connection_->options().accessKey});
  }
  reader_ = reader;

  // Cache the authoritative row count (total across all splits) once. -1 means
  // "unknown", the same sentinel the CSV supplier uses when it does not count.
  char* countError = nullptr;
  const long long records = odps_glue_reader_record_count(reader, &countError);
  odps_glue_free_string(countError);
  row_num_ = (records >= 0) ? static_cast<int64_t>(records) : -1;
}

OdpsRecordBatchSupplier::~OdpsRecordBatchSupplier() {
  // Close the reader in the destructor body (before members unwind) so it is
  // torn down while `connection_` — which owns the SDK objects it references —
  // is still alive.
  if (reader_ != nullptr) {
    odps_glue_reader_close(static_cast<OdpsGlueReader*>(reader_));
    reader_ = nullptr;
  }
}

std::shared_ptr<DataChunk> OdpsRecordBatchSupplier::GetNextChunk() {
  if (reader_ == nullptr) {
    THROW_RUNTIME_ERROR("ODPS_SCAN: reader is not open");
  }

  // Caller-owned C Data Interface structs with the canonical ArrowArray /
  // ArrowSchema layout; the glue fills them (void* keeps the seam Arrow-free).
  OdpsArrowArray array{};
  OdpsArrowSchema schema{};
  char* error = nullptr;
  const int rc = odps_glue_reader_next_batch(
      static_cast<OdpsGlueReader*>(reader_), &array, &schema, &error);
  if (rc < 0) {
    const std::string message = (error != nullptr) ? error : "unknown error";
    odps_glue_free_string(error);
    OdpsError::throwAttributed(
        "ODPS_SCAN: failed to read the next batch", message,
        {connection_->options().accessId, connection_->options().accessKey});
  }
  if (rc == 0) {
    return nullptr;  // end of data: every split exhausted
  }

  // rc == 1: convert the exported batch into an owned DataChunk, then release
  // the C structs (the guard also covers an exception from the converter).
  ArrowCReleaseGuard guard{&array, &schema};
  return recordBatchToDataChunk(schema, array);
}

#else  // ODPS_SDK_ENABLE_ARROW not defined

OdpsRecordBatchSupplier::OdpsRecordBatchSupplier(
    std::shared_ptr<reader::ReadSharedState> state) {
  (void) state;
  THROW_INVALID_ARGUMENT_EXCEPTION(
      "ODPS_SCAN: reading ODPS tables requires the ODPS SDK build (configure "
      "with -DNEUG_WITH_ODPS_SDK=ON). The pure type-mapping core and the Arrow "
      "bridge are available, but the data plane cannot be reached without the "
      "SDK.");
}

OdpsRecordBatchSupplier::~OdpsRecordBatchSupplier() = default;

std::shared_ptr<DataChunk> OdpsRecordBatchSupplier::GetNextChunk() {
  THROW_INVALID_ARGUMENT_EXCEPTION(
      "ODPS_SCAN: reading ODPS tables requires the ODPS SDK build (configure "
      "with -DNEUG_WITH_ODPS_SDK=ON).");
}

#endif  // ODPS_SDK_ENABLE_ARROW

}  // namespace odps
}  // namespace extension
}  // namespace neug
