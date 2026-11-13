/** Copyright 2020 Alibaba Group Holding Limited.
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

// Performance microbenchmark for row-group logical compaction.
//
// Focus: the per-group append frontier's vid-allocation overhead versus the
// baseline global counter. In the initial dense state the frontier is disabled
// (null) and the insert path is byte-for-byte the baseline global-counter
// branch, so there is no overhead until a compaction enables the frontier.
// This benchmark quantifies (a) the raw allocation primitive cost, single- and
// multi-threaded, and (b) the VertexTable insert throughput before vs after a
// compaction enables the frontier, plus (c) the compaction (remap) cost itself.
//
// Cadence note: VertexTable inserts use a single EnsureCapacity (geometric
// growth handled internally) followed by single-row writes into reserved slots,
// matching the production insert path -- NOT a per-row resize (which would
// trigger a whole-page merge+split and fabricate a huge false cost).
//
// Standalone executable (not registered with ctest). Run manually:
//   build/tests/storage/bench_row_group_compaction
// Scale via NEUG_RGBENCH_ALLOC_OPS / NEUG_RGBENCH_VT_ROWS / NEUG_RGBENCH_THREADS.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "neug/common/types/value.h"
#include "neug/storages/checkpoint_manager.h"
#include "neug/storages/chunk/row_group.h"
#include "neug/storages/graph/schema.h"
#include "neug/storages/graph/vertex_table.h"
#include "neug/utils/property/group_frontier.h"

namespace {

using Clock = std::chrono::steady_clock;

size_t EnvOr(const char* name, size_t def) {
  if (const char* v = std::getenv(name)) {
    return static_cast<size_t>(std::strtoull(v, nullptr, 10));
  }
  return def;
}

double MsSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// --- (a) raw allocation primitive: baseline counter vs frontier -------------

double BenchBaselineCounter(size_t ops) {
  std::atomic<uint64_t> counter{0};
  const auto t0 = Clock::now();
  for (size_t i = 0; i < ops; ++i) {
    counter.fetch_add(1, std::memory_order_relaxed);
  }
  const double ms = MsSince(t0);
  // Prevent the loop from being optimized away.
  if (counter.load() == 0) std::printf("unexpected\n");
  return ms * 1e6 / static_cast<double>(ops);  // ns/op
}

double BenchFrontier(size_t ops, size_t groups) {
  neug::RowGroupLayout layout;  // default rows_per_group
  neug::GroupFrontier frontier(layout);
  frontier.Init(groups);
  const auto t0 = Clock::now();
  for (size_t i = 0; i < ops; ++i) {
    frontier.AllocateNext();
  }
  const double ms = MsSince(t0);
  return ms * 1e6 / static_cast<double>(ops);  // ns/op
}

double BenchBaselineCounterMT(size_t ops, int threads) {
  std::atomic<uint64_t> counter{0};
  const size_t per = ops / threads;
  const auto t0 = Clock::now();
  std::vector<std::thread> ws;
  for (int t = 0; t < threads; ++t) {
    ws.emplace_back([&] {
      for (size_t i = 0; i < per; ++i) {
        counter.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& w : ws) w.join();
  const double ms = MsSince(t0);
  if (counter.load() == 0) std::printf("unexpected\n");
  return ms * 1e6 / static_cast<double>(per * threads);
}

double BenchFrontierMT(size_t ops, int threads, size_t groups) {
  neug::RowGroupLayout layout;
  neug::GroupFrontier frontier(layout);
  frontier.Init(groups);
  const size_t per = ops / threads;
  const auto t0 = Clock::now();
  std::vector<std::thread> ws;
  for (int t = 0; t < threads; ++t) {
    ws.emplace_back([&] {
      for (size_t i = 0; i < per; ++i) {
        frontier.AllocateNext();
      }
    });
  }
  for (auto& w : ws) w.join();
  const double ms = MsSince(t0);
  return ms * 1e6 / static_cast<double>(per * threads);
}

// --- (b)/(c) VertexTable insert throughput + compaction cost ----------------

struct VtResult {
  double baseline_ns_per_insert = 0;
  double frontier_ns_per_insert = 0;
  double compact_ms = 0;
  size_t compacted_valid = 0;
};

VtResult BenchVertexTable(size_t rows) {
  VtResult r;
  const std::string dir = "/tmp/bench_row_group_compaction_vt";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  neug::Schema schema;
  schema.AddVertexLabel("item", {neug::DataTypeId::kInt64}, {"val"},
                        {std::make_tuple(neug::DataTypeId::kInt64, "id", 0)},
                        4096, "", {neug::Value::INT64(0)});
  const auto label = schema.get_vertex_label_id("item");

  neug::CheckpointManager ws;
  ws.Open(dir);
  // Create a fresh checkpoint to open the table against.
  auto staging = ws.CreateStaging();
  neug::CheckpointManifest manifest;
  manifest.SetSchema(schema);
  staging.checkpoint()->SetManifest(std::move(manifest));
  auto checkpoint = staging.Publish();

  neug::VertexTable table(schema.get_vertex_schema(label));
  table.Init(checkpoint, neug::MemoryLevel::kInMemory);
  table.EnsureCapacity(rows * 2);

  // Phase A: baseline inserts (frontier disabled == production dense state).
  const auto ta0 = Clock::now();
  for (size_t i = 0; i < rows; ++i) {
    neug::vid_t vid = 0;
    table.AddVertex(neug::Value::INT64(static_cast<int64_t>(i)),
                    {neug::Value::INT64(static_cast<int64_t>(i))}, vid, 1,
                    false);
  }
  const double baseline_ms = MsSince(ta0);
  r.baseline_ns_per_insert = baseline_ms * 1e6 / static_cast<double>(rows);

  // Punch holes (delete every other row) then compact -> enables frontier.
  for (size_t i = 1; i < rows; i += 2) {
    table.DeleteVertex(static_cast<neug::vid_t>(i), neug::MAX_TIMESTAMP);
  }
  const auto tc0 = Clock::now();
  // Compact via the vertex table's group reorder (frontier enable + retreat).
  const auto stats = table.group_hole_stats(0, neug::MAX_TIMESTAMP);
  auto& frontier = table.ensure_group_frontier();
  const auto layout = table.row_group_layout();
  const auto mapping = [&] {
    // Build the reorder map for group 0 inline (mirrors the orchestrator).
    const size_t begin = layout.GroupBegin(0);
    const size_t end = layout.GroupEnd(0, table.LidNum());
    std::vector<neug::vid_t> m(end - begin,
                               std::numeric_limits<neug::vid_t>::max());
    size_t next = 0;
    const auto& vts = table.get_vertex_timestamp();
    for (size_t off = 0; off < end - begin; ++off) {
      if (vts.IsVertexValid(static_cast<neug::vid_t>(begin + off),
                            neug::MAX_TIMESTAMP)) {
        m[off] = static_cast<neug::vid_t>(begin + next);
        ++next;
      }
    }
    return m;
  }();
  table.RemapGroup(0, mapping);
  frontier.Retreat(0, stats.valid_count);
  r.compact_ms = MsSince(tc0);
  r.compacted_valid = stats.valid_count;

  // Phase B: frontier-enabled inserts (refill reclaimed tail holes).
  const size_t refill = rows / 2;
  const auto tb0 = Clock::now();
  for (size_t i = 0; i < refill; ++i) {
    neug::vid_t vid = 0;
    table.AddVertex(
        neug::Value::INT64(static_cast<int64_t>(rows + i)),
        {neug::Value::INT64(static_cast<int64_t>(rows + i))}, vid, 1, false);
  }
  const double frontier_ms = MsSince(tb0);
  r.frontier_ns_per_insert = frontier_ms * 1e6 / static_cast<double>(refill);

  table.Close();
  ws.Close();
  std::filesystem::remove_all(dir);
  return r;
}

}  // namespace

int main() {
  const size_t alloc_ops = EnvOr("NEUG_RGBENCH_ALLOC_OPS", 5ull << 20);
  const size_t vt_rows = EnvOr("NEUG_RGBENCH_VT_ROWS", 200000);
  const int threads = static_cast<int>(EnvOr("NEUG_RGBENCH_THREADS", 8));
  const size_t groups = 16;

  std::printf("\n=== Row-group compaction: vid-allocation overhead ===\n");
  std::printf("alloc_ops=%zu vt_rows=%zu threads=%d groups=%zu\n\n", alloc_ops,
              vt_rows, threads, groups);

  std::printf("[a] allocation primitive (ns/op, lower is better)\n");
  std::printf("  baseline counter  (1 thread): %8.2f\n",
              BenchBaselineCounter(alloc_ops));
  std::printf("  frontier          (1 thread): %8.2f\n",
              BenchFrontier(alloc_ops, groups));
  std::printf("  baseline counter  (%d thread): %8.2f\n", threads,
              BenchBaselineCounterMT(alloc_ops, threads));
  std::printf("  frontier          (%d thread): %8.2f\n", threads,
              BenchFrontierMT(alloc_ops, threads, groups));

  std::printf("\n[b] VertexTable insert throughput (ns/insert)\n");
  auto vt = BenchVertexTable(vt_rows);
  std::printf("  baseline (frontier off): %8.2f\n", vt.baseline_ns_per_insert);
  std::printf("  frontier (post-compact): %8.2f\n", vt.frontier_ns_per_insert);
  const double delta =
      vt.frontier_ns_per_insert - vt.baseline_ns_per_insert;
  std::printf("  frontier overhead delta: %8.2f ns/insert\n", delta);

  std::printf("\n[c] compaction (group-0 remap) cost\n");
  std::printf("  compact_ms=%.3f valid_rows=%zu\n", vt.compact_ms,
              vt.compacted_valid);

  std::printf("\nNote: initial dense state uses the baseline counter branch\n"
              "(frontier disabled); overhead applies only after compaction\n"
              "enables the frontier. Single-run wall numbers are noisy;\n"
              "compare relative deltas, not absolute values.\n");
  return 0;
}