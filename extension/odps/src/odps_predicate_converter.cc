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

#include "odps_predicate_converter.h"

#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stack>
#include <string>
#include <utility>

namespace neug {
namespace extension {
namespace odps {
namespace {

// A value-stack entry: a rendered ODPS predicate/operand fragment, or nullopt
// when the sub-expression is outside the whitelist. nullopt propagates through
// every operator, so one unsupported node makes the whole result unpushable.
using Fragment = std::optional<std::string>;

// Operator binding, lower number binds tighter (mirrors parquet's
// ArrowOperatorPrecedence convention). NOT/ISNULL are prefix unary and bind
// tightest; comparisons (including IN) next; AND/OR loosest. Unsupported
// logical/arithmetic operators keep a binary-level precedence so the shunting
// yard stays balanced while their result collapses to nullopt.
int precedenceOf(const ::common::ExprOpr& op) {
  if (op.item_case() == ::common::ExprOpr::kLogical) {
    switch (op.logical()) {
    case ::common::Logical::NOT:
    case ::common::Logical::ISNULL:
      return 2;
    case ::common::Logical::AND:
      return 11;
    case ::common::Logical::OR:
      return 12;
    default:
      return 6;  // EQ/NE/LT/LE/GT/GE/WITHIN/WITHOUT and unsupported binaries
    }
  }
  if (op.item_case() == ::common::ExprOpr::kArith) {
    return 5;  // arithmetic is unsupported -> collapses to nullopt
  }
  return 16;
}

bool isPrefixUnary(const ::common::ExprOpr& op) {
  return op.item_case() == ::common::ExprOpr::kLogical &&
         (op.logical() == ::common::Logical::NOT ||
          op.logical() == ::common::Logical::ISNULL);
}

// MaxCompute treats a string literal as single-quoted with backslash escapes;
// a single quote may also be doubled (ANSI). Escape both `\` and `'` in one
// pass so the literal round-trips and cannot break out of its quotes.
std::string quoteString(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '\'';
  for (const char c : s) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '\'') {
      out += "''";
    } else {
      out += c;
    }
  }
  out += '\'';
  return out;
}

// Render a finite floating literal with enough digits to round-trip. NaN/Inf
// are not pushed (their SQL semantics are dialect-specific).
template <typename T>
bool renderFloat(T value, std::string& out) {
  if (!std::isfinite(value)) {
    return false;
  }
  std::ostringstream oss;
  oss << std::setprecision(std::numeric_limits<T>::max_digits10) << value;
  out = oss.str();
  return true;
}

// Render a WITHIN/WITHOUT operand list (`to_list`/`to_array`) as `(v1, v2)`.
// Every element must be a plain literal; an empty list is rejected so we never
// emit a degenerate `in ()`.
bool renderList(const ::common::ExprOpr& token, std::string& out) {
  const auto* fields = token.item_case() == ::common::ExprOpr::kToList
                           ? &token.to_list().fields()
                           : &token.to_array().fields();
  if (fields->empty()) {
    return false;
  }
  std::string joined = "(";
  bool first = true;
  for (const auto& field : *fields) {
    if (field.operators_size() != 1 ||
        field.operators(0).item_case() != ::common::ExprOpr::kConst) {
      return false;
    }
    std::string literal;
    if (!OdpsPredicateConverter::renderLiteral(field.operators(0).const_(),
                                               literal)) {
      return false;
    }
    if (!first) {
      joined += ", ";
    }
    joined += literal;
    first = false;
  }
  joined += ")";
  out = std::move(joined);
  return true;
}

// Pop one operator, combine the top value(s) and push the rendered fragment.
// Returns false only on a structural error (stack underflow), which aborts the
// whole conversion. An unsupported operator yields nullopt, not an error.
bool applyOperator(const ::common::ExprOpr& op, std::stack<Fragment>& values) {
  const bool unary = isPrefixUnary(op);
  if (values.empty()) {
    return false;
  }
  Fragment right = values.top();
  values.pop();
  Fragment left;
  if (!unary) {
    if (values.empty()) {
      return false;
    }
    left = values.top();
    values.pop();
  }

  Fragment result;  // nullopt unless every operand rendered and op is supported
  if (unary) {
    if (right.has_value()) {
      result = op.logical() == ::common::Logical::NOT
                   ? Fragment("not (" + *right + ")")
                   : Fragment(*right + " is null");
    }
  } else if (left.has_value() && right.has_value() &&
             op.item_case() == ::common::ExprOpr::kLogical) {
    switch (op.logical()) {
    case ::common::Logical::EQ:
      result = *left + " = " + *right;
      break;
    case ::common::Logical::NE:
      result = *left + " != " + *right;
      break;
    case ::common::Logical::LT:
      result = *left + " < " + *right;
      break;
    case ::common::Logical::LE:
      result = *left + " <= " + *right;
      break;
    case ::common::Logical::GT:
      result = *left + " > " + *right;
      break;
    case ::common::Logical::GE:
      result = *left + " >= " + *right;
      break;
    case ::common::Logical::WITHIN:
      result = *left + " in " + *right;
      break;
    case ::common::Logical::WITHOUT:
      result = *left + " not in " + *right;
      break;
    case ::common::Logical::AND:
      result = "(" + *left + ") and (" + *right + ")";
      break;
    case ::common::Logical::OR:
      result = "(" + *left + ") or (" + *right + ")";
      break;
    default:
      break;  // STARTSWITH/ENDSWITH/REGEX/... -> nullopt (not pushed)
    }
  }
  values.push(std::move(result));
  return true;
}

}  // namespace

std::string OdpsPredicateConverter::quoteIdentifier(const std::string& name) {
  // Matches the SDK's IAttribute::ToString: backtick-quoted, internal
  // backticks doubled.
  std::string out;
  out.reserve(name.size() + 2);
  out += '`';
  for (const char c : name) {
    if (c == '`') {
      out += "``";
    } else {
      out += c;
    }
  }
  out += '`';
  return out;
}

bool OdpsPredicateConverter::renderLiteral(const ::common::Value& value,
                                           std::string& out) {
  switch (value.item_case()) {
  case ::common::Value::kBoolean:
    out = value.boolean() ? "true" : "false";
    return true;
  case ::common::Value::kI32:
    out = std::to_string(value.i32());
    return true;
  case ::common::Value::kI64:
    out = std::to_string(value.i64());
    return true;
  case ::common::Value::kU32:
    out = std::to_string(value.u32());
    return true;
  case ::common::Value::kU64:
    out = std::to_string(value.u64());
    return true;
  case ::common::Value::kF32:
    return renderFloat(value.f32(), out);
  case ::common::Value::kF64:
    return renderFloat(value.f64(), out);
  case ::common::Value::kStr:
    out = quoteString(value.str());
    return true;
  default:
    // blob / none / date / time / timestamp / legacy arrays: outside the v1
    // literal whitelist. The caller falls back to engine-side filtering.
    return false;
  }
}

OdpsPredicateConversion OdpsPredicateConverter::convert(
    const ::common::Expression& expr) {
  OdpsPredicateConversion out;  // {false, ""}
  if (expr.operators().empty()) {
    return out;
  }

  std::stack<Fragment> values;
  std::stack<::common::ExprOpr> ops;
  bool failed = false;

  auto applyTop = [&]() -> bool {
    if (ops.empty()) {
      return false;
    }
    const ::common::ExprOpr op = ops.top();
    if (!applyOperator(op, values)) {
      return false;
    }
    ops.pop();
    return true;
  };
  auto drainTighterThan = [&](int precedence) {
    while (!ops.empty() && ops.top().item_case() != ::common::ExprOpr::kBrace &&
           precedenceOf(ops.top()) <= precedence) {
      if (!applyTop()) {
        failed = true;
        return;
      }
    }
  };

  for (const auto& token : expr.operators()) {
    switch (token.item_case()) {
    case ::common::ExprOpr::kVar:
      if (token.var().tag().has_name() && !token.var().has_property()) {
        values.push(quoteIdentifier(token.var().tag().name()));
      } else {
        values.push(std::nullopt);  // nested property / id-only -> not pushed
      }
      break;
    case ::common::ExprOpr::kConst: {
      std::string literal;
      if (renderLiteral(token.const_(), literal)) {
        values.push(std::move(literal));
      } else {
        values.push(std::nullopt);
      }
      break;
    }
    case ::common::ExprOpr::kToList:
    case ::common::ExprOpr::kToArray: {
      std::string list;
      if (renderList(token, list)) {
        values.push(std::move(list));
      } else {
        values.push(std::nullopt);
      }
      break;
    }
    case ::common::ExprOpr::kParam:
      // v1 does not resolve dynamic params; the engine-side filter (which has
      // the ParamsMap) still applies them. See header note.
      values.push(std::nullopt);
      break;
    case ::common::ExprOpr::kBrace:
      if (token.brace() == ::common::ExprOpr::LEFT_BRACE) {
        ops.push(token);
      } else {
        while (!ops.empty() &&
               ops.top().item_case() != ::common::ExprOpr::kBrace) {
          if (!applyTop()) {
            failed = true;
            break;
          }
        }
        if (failed) {
          break;
        }
        if (ops.empty()) {
          failed = true;  // right brace without a matching left brace
          break;
        }
        ops.pop();  // discard the left brace
      }
      break;
    case ::common::ExprOpr::kLogical:
      if (isPrefixUnary(token)) {
        ops.push(token);  // binds to the operand that follows
        break;
      }
      drainTighterThan(precedenceOf(token));
      if (!failed) {
        ops.push(token);
      }
      break;
    case ::common::ExprOpr::kArith:
      // Arithmetic is outside the whitelist, but it is a binary operator: run
      // it through the yard so operand counts stay balanced; it yields nullopt.
      drainTighterThan(precedenceOf(token));
      if (!failed) {
        ops.push(token);
      }
      break;
    default:
      // scalar_func / case / udf / extract / map / path nodes: structure we do
      // not model. Give up on pushdown for the whole expression.
      failed = true;
      break;
    }
    if (failed) {
      break;
    }
  }

  if (failed) {
    return out;
  }
  while (!ops.empty()) {
    if (ops.top().item_case() == ::common::ExprOpr::kBrace) {
      return out;  // unmatched left brace
    }
    if (!applyTop()) {
      return out;
    }
  }
  if (values.size() != 1) {
    return out;
  }
  const Fragment& top = values.top();
  if (!top.has_value() || top->empty()) {
    return out;
  }
  out.fullyPushed = true;
  out.predicate = *top;
  return out;
}

}  // namespace odps
}  // namespace extension
}  // namespace neug
