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

#pragma once

#include <string>
#include <vector>

#include "neug/compiler/parser/expression/parsed_expression.h"
#include "neug/compiler/parser/scan_source.h"
#include "neug/compiler/parser/statement.h"

namespace neug {
namespace parser {

class LoadAs : public Statement {
 public:
  LoadAs(std::unique_ptr<BaseScanSource> source, std::string targetLabel,
         bool isEdge)
      : Statement{common::StatementType::LOAD_AS},
        source{std::move(source)},
        targetLabel{std::move(targetLabel)},
        isEdge{isEdge} {}

  BaseScanSource* getSource() const { return source.get(); }
  const std::string& getTargetLabel() const { return targetLabel; }
  bool isEdgeLoad() const { return isEdge; }

  void setParsingOption(options_t options) {
    parsingOptions = std::move(options);
  }
  const options_t& getParsingOptions() const { return parsingOptions; }

  // Node-specific
  void setPrimaryKey(std::string key) { primaryKey = std::move(key); }
  const std::string& getPrimaryKey() const { return primaryKey; }

  // Edge-specific
  void setFromLabel(std::string label) { fromLabel = std::move(label); }
  const std::string& getFromLabel() const { return fromLabel; }
  void setToLabel(std::string label) { toLabel = std::move(label); }
  const std::string& getToLabel() const { return toLabel; }
  void setFromCol(std::string col) { fromCol = std::move(col); }
  const std::string& getFromCol() const { return fromCol; }
  void setToCol(std::string col) { toCol = std::move(col); }
  const std::string& getToCol() const { return toCol; }

  // Optional WHERE clause (filter pushdown)
  void setWherePredicate(std::unique_ptr<ParsedExpression> predicate) {
    wherePredicate = std::move(predicate);
  }
  bool hasWherePredicate() const { return wherePredicate != nullptr; }
  const ParsedExpression* getWherePredicate() const {
    return wherePredicate.get();
  }

  // Optional RETURN columns (projection pushdown)
  void setReturnColumns(std::vector<std::string> columns) {
    returnColumns = std::move(columns);
  }
  bool hasReturnColumns() const { return !returnColumns.empty(); }
  const std::vector<std::string>& getReturnColumns() const {
    return returnColumns;
  }

 private:
  std::unique_ptr<BaseScanSource> source;
  std::string targetLabel;
  bool isEdge;
  options_t parsingOptions;

  // Node-specific
  std::string primaryKey;

  // Edge-specific
  std::string fromLabel;
  std::string toLabel;
  std::string fromCol;
  std::string toCol;

  // Optional clauses
  std::unique_ptr<ParsedExpression> wherePredicate;
  std::vector<std::string> returnColumns;
};

}  // namespace parser
}  // namespace neug
