/** Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "neug/execution/execute/ops/retrieve/gds_algo.h"

#include "neug/common/types.h"
#include "neug/compiler/function/gds/gds_algo_function.h"
#include "neug/compiler/main/metadata_registry.h"
#include "neug/execution/common/columns/vertex_columns.h"
#include "neug/execution/common/context.h"
#include "neug/storages/graph/graph_interface.h"
#include "neug/storages/graph/merged_storage_view.h"
#include "neug/utils/exception/exception.h"

namespace neug {
namespace execution {
namespace ops {

GDSAlgoOpr::GDSAlgoOpr(std::unique_ptr<function::CallFuncInputBase> algo_input,
                       function::GDSAlgoFunction* algo_func,
                       std::vector<label_t> vertex_labels,
                       std::vector<execution::LabelTriplet> edge_triplets,
                       bool is_multi_label)
    : algo_input_(std::move(algo_input)),
      algo_func_(algo_func),
      vertex_labels_(std::move(vertex_labels)),
      edge_triplets_(std::move(edge_triplets)),
      is_multi_label_(is_multi_label) {}

neug::result<neug::execution::Context> GDSAlgoOpr::Eval(
    IStorageInterface& graph_interface, const ParamsMap& params,
    neug::execution::Context&& ctx, neug::execution::OprTimer* timer) {
  (void) params;
  (void) timer;
  if (algo_func_ == nullptr) {
    THROW_RUNTIME_ERROR("GDSAlgoOpr: GDSAlgoFunction pointer is null");
  }
  if (algo_func_->execFunc == nullptr) {
    THROW_RUNTIME_ERROR(
        "GDSAlgoOpr: algoExec not registered for GDS algorithm");
  }
  if (algo_input_ == nullptr) {
    THROW_RUNTIME_ERROR("GDSAlgoOpr: algo input is null");
  }

  if (is_multi_label_) {
    auto& base_graph =
        dynamic_cast<StorageReadInterface&>(graph_interface);
    MergedStorageView merged(base_graph, vertex_labels_, edge_triplets_);
    auto result = algo_func_->execFunc(*algo_input_, merged);

    // Unmap global vids back to (label, local_vid) in vertex columns.
    const auto& offset_table = merged.offset_table();
    execution::Context unmapped_ctx;
    unmapped_ctx.tag_ids = result.tag_ids;
    for (size_t ci = 0; ci < result.chunk_num(); ++ci) {
      auto& chunk = result.chunk(ci);
      execution::ContextChunk new_chunk;
      for (int tag : result.tag_ids) {
        auto col = chunk.get(tag);
        if (col && col->column_type() == execution::ContextColumnType::kVertex) {
          // Rebuild vertex column with correct (label, local_vid).
          auto* vcol = dynamic_cast<const execution::IVertexColumn*>(col.get());
          execution::MLVertexColumnBuilder builder;
          builder.reserve(vcol->size());
          for (size_t i = 0; i < vcol->size(); ++i) {
            auto vr = vcol->get_vertex(i);
            auto [label_idx, local_vid] = offset_table.global_to_local(vr.vid_);
            label_t real_label = offset_table.range(label_idx).label;
            builder.push_back_vertex(execution::VertexRecord(real_label, local_vid));
          }
          new_chunk.set(tag, builder.finish());
        } else if (col) {
          new_chunk.set(tag, col);
        }
      }
      unmapped_ctx.append_chunk(std::move(new_chunk));
    }
    return unmapped_ctx;
  }
  return algo_func_->execFunc(*algo_input_, graph_interface);
}

neug::result<OpBuildResultT> GDSAlgoOprBuilder::Build(
    const neug::Schema& schema, const ContextMeta& ctx_meta,
    const physical::PhysicalPlan& plan, int op_idx) {
  auto gCatalog = neug::main::MetadataRegistry::getCatalog();
  const auto& gds_pb = plan.plan(op_idx).opr().gds_algo();
  const std::string& algo_name = gds_pb.algo_name();
  auto* func = gCatalog->getFunctionWithSignature(algo_name);
  if (func == nullptr) {
    THROW_RUNTIME_ERROR("GDSAlgoOprBuilder: GDS function not found: " +
                        algo_name);
  }
  auto* algo_func = dynamic_cast<function::GDSAlgoFunction*>(func);
  if (algo_func == nullptr) {
    THROW_RUNTIME_ERROR("GDSAlgoOprBuilder: function is not GDSAlgoFunction: " +
                        algo_name);
  }

  if (algo_func->bindFunc == nullptr) {
    THROW_RUNTIME_ERROR(
        "GDSAlgoOprBuilder: bind function not registered for GDS algorithm");
  }

  auto algo_input = algo_func->bindFunc(schema, ctx_meta, plan, op_idx);
  if (algo_input == nullptr) {
    THROW_RUNTIME_ERROR("GDSAlgoOprBuilder: algo input is null");
  }

  // Detect multi-label subgraph.
  const auto& subgraph = gds_pb.sub_graph();
  std::vector<label_t> vertex_labels;
  std::vector<execution::LabelTriplet> edge_triplets;
  bool is_multi_label = subgraph.vertex_entries_size() > 1 ||
                        subgraph.edge_entries_size() > 1;
  if (is_multi_label) {
    for (const auto& ve : subgraph.vertex_entries()) {
      vertex_labels.push_back(static_cast<label_t>(ve.label_id()));
    }
    for (const auto& ee : subgraph.edge_entries()) {
      edge_triplets.emplace_back(static_cast<label_t>(ee.src_label_id()),
                                 static_cast<label_t>(ee.dst_label_id()),
                                 static_cast<label_t>(ee.edge_label_id()));
    }
  }

  ContextMeta ret_meta = ctx_meta;
  for (int i = 0; i < plan.plan(op_idx).meta_data_size(); ++i) {
    const auto& meta = plan.plan(op_idx).meta_data(i);
    ret_meta.set(meta.alias(), parse_from_ir_data_type(meta.type()));
  }
  return std::make_pair(
      std::make_unique<GDSAlgoOpr>(std::move(algo_input), algo_func,
                                   std::move(vertex_labels),
                                   std::move(edge_triplets), is_multi_label),
      ret_meta);
}

}  // namespace ops
}  // namespace execution
}  // namespace neug
