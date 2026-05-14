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

#include "iceberg_predicate_pruner.h"

#include <cstring>
#include <optional>
#include <stack>

#include <glog/logging.h>

#include "neug/generated/proto/plan/common.pb.h"
#include "neug/generated/proto/plan/expr.pb.h"

namespace neug {
namespace iceberg {

// ── helpers ──────────────────────────────────────────────────────────────

namespace {

// Operator precedence (matches ArrowOperatorPrecedence in core)
int precedence(const ::common::ExprOpr& opr) {
  if (opr.item_case() == ::common::ExprOpr::kLogical) {
    switch (opr.logical()) {
    case ::common::Logical::NOT: return 2;
    case ::common::Logical::EQ:
    case ::common::Logical::NE: return 7;
    case ::common::Logical::LT:
    case ::common::Logical::LE:
    case ::common::Logical::GT:
    case ::common::Logical::GE: return 6;
    case ::common::Logical::AND: return 11;
    case ::common::Logical::OR: return 12;
    default: return 16;
    }
  }
  if (opr.item_case() == ::common::ExprOpr::kBrace) return 17;
  return 16;
}

bool isComparisonOp(::common::Logical op) {
  return op == ::common::Logical::EQ || op == ::common::Logical::NE ||
         op == ::common::Logical::LT || op == ::common::Logical::LE ||
         op == ::common::Logical::GT || op == ::common::Logical::GE;
}

// Flip a comparison operator (used when const is on the LHS).
::common::Logical flipOp(::common::Logical op) {
  switch (op) {
  case ::common::Logical::LT: return ::common::Logical::GT;
  case ::common::Logical::LE: return ::common::Logical::GE;
  case ::common::Logical::GT: return ::common::Logical::LT;
  case ::common::Logical::GE: return ::common::Logical::LE;
  default: return op;  // EQ, NE are symmetric
  }
}

}  // namespace

// ── constructor ──────────────────────────────────────────────────────────

IcebergPredicatePruner::IcebergPredicatePruner(
    const ::common::Expression& predicate,
    const std::vector<IcebergField>& schema,
    const std::vector<IcebergPartitionField>& partition) {
  // Build lookup maps
  for (const auto& f : schema) {
    name_to_field_id_[f.name] = f.id;
    field_id_to_type_[f.id] = f.type;
  }
  for (size_t i = 0; i < partition.size(); ++i) {
    const auto& pf = partition[i];
    if (pf.transform == "identity") {
      // Find the source column name
      for (const auto& f : schema) {
        if (f.id == pf.source_id) {
          partition_col_to_idx_[f.name] = i;
          break;
        }
      }
      partition_source_to_idx_[pf.source_id] = i;
    }
  }
  root_ = parseExpression(predicate);
}

// ── expression parsing (shunting-yard → tree) ────────────────────────────

IcebergPredicatePruner::ExprNode
IcebergPredicatePruner::parseExpression(const ::common::Expression& expr) {
  using Node = ExprNode;
  std::stack<Node> vals;
  std::stack<::common::ExprOpr> ops;

  auto apply = [&](const ::common::ExprOpr& opr) {
    if (opr.item_case() != ::common::ExprOpr::kLogical) {
      vals.push(Node{.kind = Node::UNSUPPORTED});
      return;
    }
    auto logical = opr.logical();

    if (logical == ::common::Logical::AND ||
        logical == ::common::Logical::OR) {
      if (vals.size() < 2) { vals.push(Node{.kind = Node::UNSUPPORTED}); return; }
      Node right = std::move(vals.top()); vals.pop();
      Node left  = std::move(vals.top()); vals.pop();
      Node n;
      n.kind = (logical == ::common::Logical::AND) ? Node::LOGICAL_AND
                                                    : Node::LOGICAL_OR;
      n.children.push_back(std::move(left));
      n.children.push_back(std::move(right));
      vals.push(std::move(n));
      return;
    }

    if (isComparisonOp(logical)) {
      if (vals.size() < 2) { vals.push(Node{.kind = Node::UNSUPPORTED}); return; }
      Node right = std::move(vals.top()); vals.pop();
      Node left  = std::move(vals.top()); vals.pop();

      // We only handle  var op const  or  const op var
      Node* var_node = nullptr;
      Node* const_node = nullptr;
      ::common::Logical effective_op = logical;

      if (left.kind == Node::VARIABLE && right.kind == Node::CONSTANT) {
        var_node = &left;
        const_node = &right;
      } else if (left.kind == Node::CONSTANT && right.kind == Node::VARIABLE) {
        var_node = &right;
        const_node = &left;
        effective_op = flipOp(logical);
      } else {
        vals.push(Node{.kind = Node::UNSUPPORTED});
        return;
      }

      Node cmp;
      cmp.kind = Node::COMPARISON;
      cmp.column_name = var_node->column_name;
      cmp.op = effective_op;
      // Store children: [0]=variable, [1]=constant  (for metadata lookup)
      Node cv;
      cv.kind = Node::CONSTANT;
      cv.const_value = const_node->const_value;
      cv.has_const_value = true;
      cmp.children.push_back(std::move(*var_node));
      cmp.children.push_back(std::move(cv));
      vals.push(std::move(cmp));
      return;
    }

    // NOT or unsupported logical
    vals.push(Node{.kind = Node::UNSUPPORTED});
  };

  for (int i = 0; i < expr.operators_size(); ++i) {
    const auto& opr = expr.operators(i);
    switch (opr.item_case()) {
    case ::common::ExprOpr::kConst: {
      Node n;
      n.kind = Node::CONSTANT;
      n.const_value = opr.const_();
      n.has_const_value = true;
      vals.push(std::move(n));
      break;
    }
    case ::common::ExprOpr::kVar: {
      Node n;
      n.kind = Node::VARIABLE;
      if (opr.var().has_tag() && opr.var().tag().has_name()) {
        n.column_name = opr.var().tag().name();
      }
      vals.push(std::move(n));
      break;
    }
    case ::common::ExprOpr::kBrace: {
      if (opr.brace() == ::common::ExprOpr::LEFT_BRACE) {
        ops.push(opr);
      } else {
        while (!ops.empty() &&
               ops.top().item_case() != ::common::ExprOpr::kBrace) {
          apply(ops.top());
          ops.pop();
        }
        if (!ops.empty()) ops.pop();  // pop LEFT_BRACE
      }
      break;
    }
    case ::common::ExprOpr::kLogical:
    case ::common::ExprOpr::kArith: {
      int cur = precedence(opr);
      while (!ops.empty() &&
             ops.top().item_case() != ::common::ExprOpr::kBrace &&
             precedence(ops.top()) <= cur) {
        apply(ops.top());
        ops.pop();
      }
      ops.push(opr);
      break;
    }
    default:
      vals.push(Node{.kind = Node::UNSUPPORTED});
      break;
    }
  }

  while (!ops.empty()) {
    apply(ops.top());
    ops.pop();
  }

  if (vals.empty()) return Node{.kind = Node::UNSUPPORTED};
  return std::move(vals.top());
}

// ── bound comparison ─────────────────────────────────────────────────────

std::optional<int> IcebergPredicatePruner::compareBoundToValue(
    const std::string& bound_bytes,
    const ::common::Value& val,
    const std::string& iceberg_type) const {
  if (iceberg_type == "long") {
    if (bound_bytes.size() < sizeof(int64_t)) return std::nullopt;
    int64_t bound;
    std::memcpy(&bound, bound_bytes.data(), sizeof(int64_t));
    int64_t pred = 0;
    if (val.item_case() == ::common::Value::kI64) pred = val.i64();
    else if (val.item_case() == ::common::Value::kI32) pred = val.i32();
    else if (val.item_case() == ::common::Value::kF64) pred = static_cast<int64_t>(val.f64());
    else return std::nullopt;
    return (bound < pred) ? -1 : (bound > pred ? 1 : 0);
  }
  if (iceberg_type == "int") {
    if (bound_bytes.size() < sizeof(int32_t)) return std::nullopt;
    int32_t bound;
    std::memcpy(&bound, bound_bytes.data(), sizeof(int32_t));
    int64_t pred = 0;
    if (val.item_case() == ::common::Value::kI32) pred = val.i32();
    else if (val.item_case() == ::common::Value::kI64) pred = val.i64();
    else if (val.item_case() == ::common::Value::kF64) pred = static_cast<int64_t>(val.f64());
    else return std::nullopt;
    int64_t b64 = bound;
    return (b64 < pred) ? -1 : (b64 > pred ? 1 : 0);
  }
  if (iceberg_type == "double") {
    if (bound_bytes.size() < sizeof(double)) return std::nullopt;
    double bound;
    std::memcpy(&bound, bound_bytes.data(), sizeof(double));
    double pred = 0;
    if (val.item_case() == ::common::Value::kF64) pred = val.f64();
    else if (val.item_case() == ::common::Value::kF32) pred = val.f32();
    else if (val.item_case() == ::common::Value::kI64) pred = static_cast<double>(val.i64());
    else if (val.item_case() == ::common::Value::kI32) pred = static_cast<double>(val.i32());
    else return std::nullopt;
    return (bound < pred) ? -1 : (bound > pred ? 1 : 0);
  }
  if (iceberg_type == "float") {
    if (bound_bytes.size() < sizeof(float)) return std::nullopt;
    float bound;
    std::memcpy(&bound, bound_bytes.data(), sizeof(float));
    double pred = 0;
    if (val.item_case() == ::common::Value::kF64) pred = val.f64();
    else if (val.item_case() == ::common::Value::kF32) pred = val.f32();
    else if (val.item_case() == ::common::Value::kI32) pred = static_cast<double>(val.i32());
    else return std::nullopt;
    double bd = bound;
    return (bd < pred) ? -1 : (bd > pred ? 1 : 0);
  }
  if (iceberg_type == "string") {
    if (val.item_case() != ::common::Value::kStr) return std::nullopt;
    int cmp = bound_bytes.compare(val.str());
    return (cmp < 0) ? -1 : (cmp > 0 ? 1 : 0);
  }
  return std::nullopt;  // unsupported type
}

// ── canSkipWithBounds ────────────────────────────────────────────────────

bool IcebergPredicatePruner::canSkipWithBounds(
    const ExprNode& node,
    const std::map<int32_t, std::string>& lower,
    const std::map<int32_t, std::string>& upper) const {

  switch (node.kind) {
  case ExprNode::LOGICAL_AND: {
    // AND: if ANY child allows skipping, the whole AND can be skipped
    for (const auto& child : node.children) {
      if (canSkipWithBounds(child, lower, upper)) return true;
    }
    return false;
  }
  case ExprNode::LOGICAL_OR: {
    // OR: ALL children must allow skipping
    for (const auto& child : node.children) {
      if (!canSkipWithBounds(child, lower, upper)) return false;
    }
    return true;
  }
  case ExprNode::COMPARISON: {
    // Extract column info
    const auto& col_name = node.column_name;
    auto it_fid = name_to_field_id_.find(col_name);
    if (it_fid == name_to_field_id_.end()) return false;
    int32_t fid = it_fid->second;

    auto it_type = field_id_to_type_.find(fid);
    if (it_type == field_id_to_type_.end()) return false;
    const auto& type = it_type->second;

    // Get constant value (stored in children[1])
    if (node.children.size() < 2 || !node.children[1].has_const_value) {
      return false;
    }
    const auto& pred_val = node.children[1].const_value;

    auto lb_it = lower.find(fid);
    auto ub_it = upper.find(fid);

    switch (node.op) {
    case ::common::Logical::GT: {
      // col > X  →  skip if upper <= X
      if (ub_it == upper.end()) return false;
      auto cmp = compareBoundToValue(ub_it->second, pred_val, type);
      return cmp.has_value() && cmp.value() <= 0;
    }
    case ::common::Logical::GE: {
      // col >= X  →  skip if upper < X
      if (ub_it == upper.end()) return false;
      auto cmp = compareBoundToValue(ub_it->second, pred_val, type);
      return cmp.has_value() && cmp.value() < 0;
    }
    case ::common::Logical::LT: {
      // col < X  →  skip if lower >= X
      if (lb_it == lower.end()) return false;
      auto cmp = compareBoundToValue(lb_it->second, pred_val, type);
      return cmp.has_value() && cmp.value() >= 0;
    }
    case ::common::Logical::LE: {
      // col <= X  →  skip if lower > X
      if (lb_it == lower.end()) return false;
      auto cmp = compareBoundToValue(lb_it->second, pred_val, type);
      return cmp.has_value() && cmp.value() > 0;
    }
    case ::common::Logical::EQ: {
      // col = X  →  skip if upper < X or lower > X
      if (ub_it != upper.end()) {
        auto cmp = compareBoundToValue(ub_it->second, pred_val, type);
        if (cmp.has_value() && cmp.value() < 0) return true;
      }
      if (lb_it != lower.end()) {
        auto cmp = compareBoundToValue(lb_it->second, pred_val, type);
        if (cmp.has_value() && cmp.value() > 0) return true;
      }
      return false;
    }
    default:
      return false;  // NE and others: don't prune (conservative)
    }
  }
  default:
    return false;  // UNSUPPORTED: conservative, never skip
  }
}

// ── public API ───────────────────────────────────────────────────────────

bool IcebergPredicatePruner::canSkipManifest(
    const ManifestListEntry& entry) const {
  if (entry.partitions.empty()) return false;

  // Build a pseudo lower/upper bounds map from partition summaries.
  // Each partition summary[i] corresponds to partition_spec[i].
  // We map from the partition source column's field_id to the summary bounds.
  std::map<int32_t, std::string> lower_bounds, upper_bounds;
  for (const auto& [source_id, idx] : partition_source_to_idx_) {
    if (idx < entry.partitions.size()) {
      const auto& summary = entry.partitions[idx];
      if (!summary.lower_bound.empty()) {
        lower_bounds[source_id] = summary.lower_bound;
      }
      if (!summary.upper_bound.empty()) {
        upper_bounds[source_id] = summary.upper_bound;
      }
    }
  }

  if (lower_bounds.empty() && upper_bounds.empty()) return false;
  return canSkipWithBounds(root_, lower_bounds, upper_bounds);
}

bool IcebergPredicatePruner::canSkipDataFile(const DataFileEntry& entry) const {
  if (entry.lower_bounds.empty() && entry.upper_bounds.empty()) return false;
  return canSkipWithBounds(root_, entry.lower_bounds, entry.upper_bounds);
}

}  // namespace iceberg
}  // namespace neug
