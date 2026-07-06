#pragma once
#include "neug/compiler/function/gds/gds_algo_function.h"
#include "neug/compiler/function/neug_call_function.h"
namespace neug {
namespace gds {
struct NEUG_API MultiLabelLouvainFunction {
  static constexpr const char* name = "multi_label_louvain";
  static std::unique_ptr<function::CallFuncInputBase> bind(
      const Schema& schema, const execution::ContextMeta& ctx_meta,
      const ::physical::PhysicalPlan& plan, int op_idx);
  static execution::Context exec(const function::CallFuncInputBase& input_base,
                                 neug::IStorageInterface& g);
  static function::function_set getFunctionSet();
};
}  // namespace gds
}  // namespace neug
