/**
 * Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "neug/compiler/parser/load_as.h"
#include "neug/compiler/parser/transformer.h"

using namespace neug::common;

namespace neug {
namespace parser {

std::unique_ptr<Statement> Transformer::transformLoadNodeTable(
    CypherParser::NEUG_LoadNodeTableContext& ctx) {
  auto source = transformScanSource(*ctx.nEUG_ScanSource());
  auto targetLabel = transformSchemaName(*ctx.oC_SchemaName());
  auto loadAs = std::make_unique<LoadAs>(std::move(source),
                                         std::move(targetLabel),
                                         /*isEdge=*/false);
  if (ctx.nEUG_Options()) {
    loadAs->setParsingOption(transformOptions(*ctx.nEUG_Options()));
  }
  if (ctx.oC_Where()) {
    loadAs->setWherePredicate(transformWhere(*ctx.oC_Where()));
  }
  if (ctx.nEUG_ReturnColumns()) {
    loadAs->setReturnColumns(
        transformReturnColumns(*ctx.nEUG_ReturnColumns()));
  }
  return loadAs;
}

std::unique_ptr<Statement> Transformer::transformLoadEdgeTable(
    CypherParser::NEUG_LoadEdgeTableContext& ctx) {
  auto source = transformScanSource(*ctx.nEUG_ScanSource());
  auto targetLabel = transformSchemaName(*ctx.oC_SchemaName());
  auto loadAs = std::make_unique<LoadAs>(std::move(source),
                                         std::move(targetLabel),
                                         /*isEdge=*/true);
  if (ctx.nEUG_Options()) {
    loadAs->setParsingOption(transformOptions(*ctx.nEUG_Options()));
  }
  if (ctx.oC_Where()) {
    loadAs->setWherePredicate(transformWhere(*ctx.oC_Where()));
  }
  if (ctx.nEUG_ReturnColumns()) {
    loadAs->setReturnColumns(
        transformReturnColumns(*ctx.nEUG_ReturnColumns()));
  }
  return loadAs;
}

std::vector<std::string> Transformer::transformReturnColumns(
    CypherParser::NEUG_ReturnColumnsContext& ctx) {
  std::vector<std::string> columns;
  for (auto& schemaName : ctx.oC_SchemaName()) {
    columns.push_back(transformSchemaName(*schemaName));
  }
  return columns;
}

}  // namespace parser
}  // namespace neug
