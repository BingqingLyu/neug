/**
 * Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include "utils/v013_compat.h"

// Compatibility shim for building the GDS extension against the NeuG
// v0.1.3 core, where execution context/column types still live in
// `neug::execution` (they were moved to `neug` upstream after v0.1.3).

#include "neug/execution/common/columns/container_types.h"
#include "neug/execution/common/columns/edge_columns.h"
#include "neug/execution/common/columns/i_context_column.h"
#include "neug/execution/common/columns/path_columns.h"
#include "neug/execution/common/columns/value_columns.h"
#include "neug/execution/common/columns/vertex_columns.h"
#include "neug/execution/common/context_chunk.h"
#include "neug/execution/common/types/graph_types.h"
#include "neug/execution/common/types/value.h"

namespace neug {

using execution::ContextChunk;
using execution::Direction;
using execution::EdgeRecord;
using execution::IContextColumn;
using execution::IEdgeColumn;
using execution::IVertexColumn;
using execution::LabelTriplet;
using execution::MLVertexColumn;
using execution::MLVertexColumnBuilder;
using execution::MSVertexColumn;
using execution::MSVertexColumnBuilder;
using execution::Path;
using execution::PathColumn;
using execution::PathColumnBuilder;
using execution::PathValue;
using execution::SLVertexColumn;
using execution::Value;
using execution::ValueColumn;
using execution::ValueColumnBuilder;
using execution::VertexRecord;

}  // namespace neug
