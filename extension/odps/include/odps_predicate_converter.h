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

#include <string>

#include "neug/generated/proto/plan/expr.pb.h"

namespace neug {
namespace extension {
namespace odps {

/// Outcome of translating a NeuG `skip_rows` filter expression into the ODPS
/// Storage API filter-predicate string dialect.
///
/// The Storage API models a pushed predicate as an `IPredicate` tree, but the
/// tree is only ever a builder for a string: the SDK sends
/// `mFilterOptions.mPredicate->ToString()` to `SetFilterPredicate`. Every
/// operand (`IAttribute`/`IConstant`/`IRawPredicate`) is itself a
/// `std::string`. So the whole pushdown reduces to producing that string, which
/// lets the translation run entirely on the ABI=1 side and cross the glue seam
/// as a plain `const char*` -- no SDK object is constructed outside the SDK
/// build.
struct OdpsPredicateConversion {
  /// True only when the ENTIRE expression translated into an equivalent ODPS
  /// predicate. When false, `predicate` is empty and the caller MUST keep
  /// engine-side filtering (`reader::filter_chunk`) to preserve correctness
  /// (spec FR-013 / task T304). When true the predicate may still be re-applied
  /// engine-side defensively; that is idempotent.
  bool fullyPushed = false;
  /// ODPS predicate string, e.g. "`age` > 30 and `ds` in ('a', 'b')". Empty
  /// unless `fullyPushed` is true.
  std::string predicate;
};

/// Translates NeuG filter expressions (`common::Expression`) into the ODPS
/// Storage API predicate string dialect.
///
/// Deliberately SDK-free (no ODPS/Arrow headers) so it compiles and is
/// unit-testable in the default `NEUG_WITH_ODPS_SDK=OFF` build. It mirrors the
/// whitelist + fallback discipline of parquet's `ArrowExpressionConverter`:
/// anything outside the supported set makes the whole expression non-pushable,
/// which is always safe because every reader re-applies `skip_rows` after
/// decoding. v1 pushes an expression whole or not at all; extracting a pushable
/// AND-conjunct prefix while keeping the residual engine-side is a documented
/// follow-up.
///
/// Supported (v1): column references (tag-only variables), scalar literals
/// (bool / signed+unsigned int / finite float / string), the comparisons
/// `=` `!=` `<` `<=` `>` `>=`, `IN` / `NOT IN` (`WITHIN` / `WITHOUT` over a
/// literal list), `IS NULL` (`ISNULL`), `NOT`, `AND`, `OR`.
///
/// Not pushed (fall back to engine-side filtering): arithmetic, dynamic params,
/// scalar/UDF functions, `CASE`, `STARTSWITH`/`ENDSWITH`/`REGEX`, date/time/
/// timestamp/blob/null literals, nested property references, and any other
/// expression node.
class OdpsPredicateConverter {
 public:
  /// Translate `expr`. Never throws: an unsupported construct yields
  /// `{false, ""}` so the caller silently keeps engine-side filtering.
  static OdpsPredicateConversion convert(const ::common::Expression& expr);

  /// ODPS identifier quoting (backtick, with internal backticks doubled),
  /// matching the SDK's `IAttribute::ToString`. Exposed for unit tests.
  static std::string quoteIdentifier(const std::string& name);

  /// Render a protobuf constant as an ODPS SQL literal into `out`. Returns
  /// false (leaving `out` untouched) for literal types outside the v1
  /// whitelist. Exposed for unit tests.
  static bool renderLiteral(const ::common::Value& value, std::string& out);
};

}  // namespace odps
}  // namespace extension
}  // namespace neug
