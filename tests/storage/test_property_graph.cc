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

#include <algorithm>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "neug/common/types/value.h"
#include "neug/storages/checkpoint_manager.h"
#include "neug/storages/graph/property_graph.h"
#include "unittest/utils.h"

namespace neug {

TEST(DirtyTrackerTest, MergeFromIncludesEdgesOnlyPresentInSource) {
  DirtyTracker target;
  target.AddEdgeSlot(1, true);

  DirtyTracker source;
  source.AddEdgeSlot(2, true);

  target.MergeFrom(source);

  EXPECT_TRUE(target.IsEdgeDirty(1));
  EXPECT_TRUE(target.IsEdgeDirty(2));
}

class PropertyGraphTest : public ::testing::Test {
 protected:
  std::string work_dir_;
  std::unique_ptr<PropertyGraph> graph_;
  CheckpointManager checkpoint_mgr_;

  void SetUp() override {
    work_dir_ = std::string("/tmp/test_property_graph") +
                ::testing::UnitTest::GetInstance()->current_test_info()->name();
    if (std::filesystem::exists(work_dir_)) {
      std::filesystem::remove_all(work_dir_);
    }
    std::filesystem::create_directories(work_dir_);
    graph_ = std::make_unique<PropertyGraph>();
    checkpoint_mgr_.Open(work_dir_);
    auto ckp = make_checkpoint(checkpoint_mgr_);
    graph_->Open(ckp, MemoryLevel::kInMemory);
  }

  void TearDown() override {
    graph_.reset();
    if (std::filesystem::exists(work_dir_)) {
      std::filesystem::remove_all(work_dir_);
    }
  }

  void CreateModernGraphSchema() {
    CreateVertexTypeParamBuilder person_builder;
    EXPECT_TRUE(graph_
                    ->CreateVertexType(
                        person_builder.VertexLabel("person")
                            .AddProperty("id", neug::Value::INT64(0))
                            .AddProperty("name", neug::Value::STRING(""))
                            .AddProperty("age", neug::Value::INT32(0))
                            .AddProperty("score", neug::Value::DOUBLE(0.0))
                            .AddPrimaryKeyName("id")
                            .Build())
                    .ok());
    CreateVertexTypeParamBuilder company_builder;
    EXPECT_TRUE(
        graph_
            ->CreateVertexType(company_builder.VertexLabel("company")
                                   .AddProperty("id", neug::Value::INT64(0))
                                   .AddProperty("name", neug::Value::STRING(""))
                                   .AddPrimaryKeyName("id")
                                   .Build())
            .ok());
    CreateEdgeTypeParamBuilder knows_builder;
    EXPECT_TRUE(graph_
                    ->CreateEdgeType(
                        knows_builder.SrcLabel("person")
                            .DstLabel("person")
                            .EdgeLabel("knows")
                            .AddProperty("weight", neug::Value::DOUBLE(0.0))
                            .Build())
                    .ok());
  }
};

TEST_F(PropertyGraphTest, TestOpenAndBulkInsert) {
  CreateModernGraphSchema();
  label_t person_label = graph_->schema().get_vertex_label_id("person");
  label_t knows_label = graph_->schema().get_edge_label_id("knows");

  vid_t vid1, vid2;
  EXPECT_TRUE(
      graph_
          ->AddVertex(person_label, neug::Value::INT64(1),
                      {neug::Value::STRING("Alice"), neug::Value::INT32(30),
                       neug::Value::DOUBLE(88.5)},
                      vid1, 0)
          .ok());
  EXPECT_TRUE(
      graph_
          ->AddVertex(person_label, neug::Value::INT64(2),
                      {neug::Value::STRING("Bob"), neug::Value::INT32(25),
                       neug::Value::DOUBLE(92.0)},
                      vid2, 0)
          .ok());
  auto id_column = graph_->GetVertexPropertyColumn(person_label, "id");
  EXPECT_TRUE(id_column);
  EXPECT_EQ(id_column->get_any(vid1).GetValue<int64_t>(), 1);
  EXPECT_EQ(id_column->get_any(vid2).GetValue<int64_t>(), 2);

  // By default, we will reserve 4096 slots for each vertex label.
  for (size_t i = 3; i <= 4096; ++i) {
    vid_t vid;
    graph_->AddVertex(person_label, neug::Value::INT64(i),
                      {neug::Value::STRING("User" + std::to_string(i)),
                       neug::Value::INT32(20 + (i % 10)),
                       neug::Value::DOUBLE(80.0 + (i % 20))},
                      vid, 0);
  }
  EXPECT_EQ(graph_->VertexNum(person_label), 4096);
  vid_t vid4097;
  EXPECT_FALSE(
      graph_
          ->AddVertex(person_label, neug::Value::INT64(4097),
                      {neug::Value::STRING("User4097"), neug::Value::INT32(27),
                       neug::Value::DOUBLE(85.0)},
                      vid4097, 0)
          .ok());

  Allocator allocator(MemoryLevel::kInMemory, "");
  for (vid_t i = 0; i < 4094; ++i) {
    int32_t oe_offset = 0;
    const void* prop = nullptr;
    graph_->AddEdge(person_label, i, person_label, i + 1, knows_label,
                    {neug::Value::DOUBLE(1.0)}, MAX_TIMESTAMP, allocator,
                    oe_offset, prop);
  }
  {
    int32_t oe_offset = 0;
    const void* prop = nullptr;
    EXPECT_FALSE(graph_
                     ->AddEdge(person_label, 4095, person_label, 4096,
                               knows_label, {neug::Value::DOUBLE(1.0)},
                               MAX_TIMESTAMP, allocator, oe_offset, prop)
                     .ok());
  }
}

TEST_F(PropertyGraphTest, DeleteMissingVerticesIsNoOp) {
  CreateModernGraphSchema();
  const auto person_label = graph_->schema().get_vertex_label_id("person");
  vid_t alice_vid;
  ASSERT_TRUE(graph_
                  ->AddVertex(person_label, Value::INT64(1),
                              {Value::STRING("Alice"), Value::INT32(30),
                               Value::DOUBLE(88.5)},
                              alice_vid, MAX_TIMESTAMP)
                  .ok());

  const auto missing_vid = std::numeric_limits<vid_t>::max();
  EXPECT_TRUE(
      graph_->DeleteVertex(person_label, Value::INT64(2), MAX_TIMESTAMP).ok());
  EXPECT_TRUE(
      graph_->DeleteVertex(person_label, missing_vid, MAX_TIMESTAMP).ok());
  EXPECT_TRUE(graph_
                  ->BatchDeleteVertices(person_label,
                                        {alice_vid, alice_vid, missing_vid})
                  .ok());
  EXPECT_TRUE(
      graph_->DeleteVertex(person_label, alice_vid, MAX_TIMESTAMP).ok());
  EXPECT_TRUE(
      graph_->DeleteVertex(person_label, Value::INT64(1), MAX_TIMESTAMP).ok());
  EXPECT_EQ(graph_->VertexNum(person_label), 0);
}

// Orchestrator-level consistency: compacting a label with
// a self-referential edge table must keep the four structures consistent —
// PK lookups, properties, and both CSR directions — and reclaim tail holes
// for refill.
TEST_F(PropertyGraphTest, CompactVertexLabelGroupsRemapsSelfEdgesAndRefills) {
  CreateModernGraphSchema();
  const auto person_label = graph_->schema().get_vertex_label_id("person");
  const auto knows_label = graph_->schema().get_edge_label_id("knows");

  // 8 persons, oid = vid = i, age = 100 + i.
  for (int64_t i = 0; i < 8; ++i) {
    vid_t vid;
    ASSERT_TRUE(graph_
                    ->AddVertex(person_label, Value::INT64(i),
                                {Value::STRING("n" + std::to_string(i)),
                                 Value::INT32(static_cast<int32_t>(100 + i)),
                                 Value::DOUBLE(static_cast<double>(i))},
                                vid, 1)
                    .ok());
    ASSERT_EQ(vid, static_cast<vid_t>(i));
  }

  // Chain 0->1->...->7 plus cross edges 7->0, 2->6, 0->4.
  Allocator allocator(MemoryLevel::kInMemory, "");
  auto add_edge = [&](vid_t src, vid_t dst) {
    int32_t oe_offset = 0;
    const void* prop = nullptr;
    ASSERT_TRUE(graph_
                    ->AddEdge(person_label, src, person_label, dst, knows_label,
                              {Value::DOUBLE(1.0)}, 1, allocator, oe_offset,
                              prop)
                    .ok());
  };
  for (vid_t i = 0; i + 1 < 8; ++i) {
    add_edge(i, i + 1);
  }
  add_edge(7, 0);
  add_edge(2, 6);
  add_edge(0, 4);

  // Delete oids 1, 3, 5 (cascades their edges). Surviving edges:
  // 6->7, 7->0, 2->6, 0->4.
  for (int64_t oid : {1, 3, 5}) {
    ASSERT_TRUE(graph_->DeleteVertex(person_label, Value::INT64(oid), 2).ok());
  }

  // Compact every group with holes (manual threshold 0).
  graph_->CompactVertexLabelGroups(person_label, 0.0);

  // Survivors gathered to the front: oid {0,2,4,6,7} -> vid {0,1,2,3,4}.
  const std::vector<int64_t> survivors = {0, 2, 4, 6, 7};
  auto age_col = graph_->GetVertexPropertyColumn(person_label, "age");
  ASSERT_TRUE(age_col);
  for (size_t i = 0; i < survivors.size(); ++i) {
    vid_t vid;
    ASSERT_TRUE(
        graph_->get_lid(person_label, Value::INT64(survivors[i]), vid, 3))
        << "oid " << survivors[i] << " lost after compaction";
    EXPECT_EQ(vid, static_cast<vid_t>(i));
    EXPECT_EQ(age_col->get_any(vid).GetValue<int32_t>(),
              static_cast<int32_t>(100 + survivors[i]));
  }
  for (int64_t oid : {1, 3, 5}) {
    vid_t vid;
    EXPECT_FALSE(graph_->get_lid(person_label, Value::INT64(oid), vid, 3));
  }

  // Both CSR directions agree with the remapped PK bindings.
  const auto& et =
      graph_->get_edge_table(person_label, person_label, knows_label);
  auto out_view = et.get_outgoing_view(MAX_TIMESTAMP);
  auto in_view = et.get_incoming_view(MAX_TIMESTAMP);
  auto has_neighbor = [](const CsrView& view, vid_t v, vid_t nbr) {
    auto nbrs = view.get_edges(v);
    for (auto it = nbrs.begin(); it != nbrs.end(); ++it) {
      if (*it == nbr) {
        return true;
      }
    }
    return false;
  };
  const std::vector<std::pair<int64_t, int64_t>> surviving_edges = {
      {6, 7}, {7, 0}, {2, 6}, {0, 4}};
  for (const auto& [src_oid, dst_oid] : surviving_edges) {
    vid_t src_vid, dst_vid;
    ASSERT_TRUE(
        graph_->get_lid(person_label, Value::INT64(src_oid), src_vid, 3));
    ASSERT_TRUE(
        graph_->get_lid(person_label, Value::INT64(dst_oid), dst_vid, 3));
    EXPECT_TRUE(has_neighbor(out_view, src_vid, dst_vid))
        << "out edge " << src_oid << "->" << dst_oid << " lost";
    EXPECT_TRUE(has_neighbor(in_view, dst_vid, src_vid))
        << "in edge " << src_oid << "->" << dst_oid << " lost";
  }
  // No phantom edges: total out-degree over survivors equals 4.
  size_t total_out = 0;
  for (vid_t v = 0; v < 5; ++v) {
    auto nbrs = out_view.get_edges(v);
    for (auto it = nbrs.begin(); it != nbrs.end(); ++it) {
      ++total_out;
    }
  }
  EXPECT_EQ(total_out, 4u);

  // Holes reclaimed: group 0 reads dense after compaction.
  auto stats = graph_->get_vertex_table(person_label).group_hole_stats(0, 3);
  EXPECT_EQ(stats.valid_count, 5u);
  EXPECT_DOUBLE_EQ(stats.hole_rate, 0.0);

  // Tail-hole refill: the next insert lands at vid 5 with a fresh PK.
  vid_t refill_vid = std::numeric_limits<vid_t>::max();
  ASSERT_TRUE(graph_
                  ->AddVertex(person_label, Value::INT64(100),
                              {Value::STRING("refill"), Value::INT32(999),
                               Value::DOUBLE(99.0)},
                              refill_vid, 4)
                  .ok());
  EXPECT_EQ(refill_vid, 5u);
  vid_t looked_up = std::numeric_limits<vid_t>::max();
  ASSERT_TRUE(graph_->get_lid(person_label, Value::INT64(100), looked_up, 4));
  EXPECT_EQ(looked_up, refill_vid);
}

// Automatic candidate selection: the hole-rate
// threshold gates which groups reorder; insert-only workloads (hole rate 0)
// never trigger, and a group below the threshold keeps its original vids.
TEST_F(PropertyGraphTest, CompactThresholdGatesCandidateSelection) {
  CreateModernGraphSchema();
  const auto person_label = graph_->schema().get_vertex_label_id("person");
  for (int64_t i = 0; i < 8; ++i) {
    vid_t vid;
    ASSERT_TRUE(graph_
                    ->AddVertex(person_label, Value::INT64(i),
                                {Value::STRING("n" + std::to_string(i)),
                                 Value::INT32(static_cast<int32_t>(100 + i)),
                                 Value::DOUBLE(static_cast<double>(i))},
                                vid, 1)
                    .ok());
    ASSERT_EQ(vid, static_cast<vid_t>(i));
  }

  // Insert-only: hole rate 0, no threshold selects the group, vids unchanged.
  graph_->CompactVertexLabelGroups(person_label, 0.3);
  for (int64_t i = 0; i < 8; ++i) {
    vid_t vid;
    ASSERT_TRUE(graph_->get_lid(person_label, Value::INT64(i), vid, 2));
    EXPECT_EQ(vid, static_cast<vid_t>(i));
  }

  // Delete 3 of 8 -> hole rate 0.375 in group 0.
  for (int64_t oid : {1, 3, 5}) {
    ASSERT_TRUE(graph_->DeleteVertex(person_label, Value::INT64(oid), 2).ok());
  }
  auto stats = graph_->get_vertex_table(person_label).group_hole_stats(0, 3);
  EXPECT_DOUBLE_EQ(stats.hole_rate, 0.375);

  // Below threshold (0.5 > 0.375): not selected, survivors keep original vids.
  graph_->CompactVertexLabelGroups(person_label, 0.5);
  const std::vector<int64_t> survivors = {0, 2, 4, 6, 7};
  for (int64_t oid : survivors) {
    vid_t vid;
    ASSERT_TRUE(graph_->get_lid(person_label, Value::INT64(oid), vid, 3));
    EXPECT_EQ(vid, static_cast<vid_t>(oid))
        << "below-threshold group must not reorder";
  }

  // At/above threshold (0.3 <= 0.375): selected, survivors gather to front.
  graph_->CompactVertexLabelGroups(person_label, 0.3);
  for (size_t i = 0; i < survivors.size(); ++i) {
    vid_t vid;
    ASSERT_TRUE(
        graph_->get_lid(person_label, Value::INT64(survivors[i]), vid, 3));
    EXPECT_EQ(vid, static_cast<vid_t>(i));
  }
  auto after = graph_->get_vertex_table(person_label).group_hole_stats(0, 3);
  EXPECT_DOUBLE_EQ(after.hole_rate, 0.0);
}

// COMPACT atomic publish / crash recovery.
// Logical compaction rides the checkpoint single publisher: the durable
// decision point is staging.Publish() (the CURRENT switch). A crash after the
// remap is dumped to staging but before Publish leaves CURRENT on the prior
// checkpoint, so recovery rolls back to the pre-compaction state and the
// orphan staging is discarded. A published COMPACT recovers the remapped
// state consistently from disk through a fresh open.
TEST_F(PropertyGraphTest, CompactPublishAtomicAndCrashBeforePublishRollsBack) {
  CreateModernGraphSchema();
  const auto person_label = graph_->schema().get_vertex_label_id("person");
  const auto knows_label = graph_->schema().get_edge_label_id("knows");

  Allocator allocator(MemoryLevel::kInMemory, "");
  for (int64_t i = 0; i < 8; ++i) {
    vid_t vid = 0;
    ASSERT_TRUE(graph_
                    ->AddVertex(person_label, Value::INT64(i),
                                {Value::STRING("n" + std::to_string(i)),
                                 Value::INT32(static_cast<int32_t>(100 + i)),
                                 Value::DOUBLE(static_cast<double>(i))},
                                vid, 1)
                    .ok());
    ASSERT_EQ(vid, static_cast<vid_t>(i));
  }
  for (vid_t i = 0; i + 1 < 8; ++i) {
    int32_t oe_offset = 0;
    const void* prop = nullptr;
    ASSERT_TRUE(graph_
                    ->AddEdge(person_label, i, person_label, i + 1, knows_label,
                              {Value::DOUBLE(1.0)}, 1, allocator, oe_offset,
                              prop)
                    .ok());
  }
  for (int64_t oid : {1, 3, 5}) {
    ASSERT_TRUE(graph_->DeleteVertex(person_label, Value::INT64(oid), 2).ok());
  }
  graph_->MarkSchemaDirty();
  graph_->MarkVertexTableDirty(person_label);
  graph_->MarkEdgeTableDirty(person_label, person_label, knows_label);

  // Baseline checkpoint A: pre-compaction state.
  graph_->Compact();
  auto staging_a = checkpoint_mgr_.CreateStaging();
  graph_->DumpAndClear(staging_a.checkpoint());
  auto checkpoint_a = staging_a.Publish();
  graph_->Open(checkpoint_a, MemoryLevel::kInMemory);

  const std::vector<int64_t> survivors = {0, 2, 4, 6, 7};
  auto expect_original_layout = [&]() {
    for (int64_t oid : survivors) {
      vid_t vid = 0;
      ASSERT_TRUE(graph_->get_lid(person_label, Value::INT64(oid), vid, 3));
      EXPECT_EQ(vid, static_cast<vid_t>(oid));
    }
    for (int64_t oid : {1, 3, 5}) {
      vid_t vid = 0;
      EXPECT_FALSE(graph_->get_lid(person_label, Value::INT64(oid), vid, 3));
    }
  };
  expect_original_layout();

  // Phase 1: remap + dump to staging B, then "crash" BEFORE Publish. CURRENT
  // must still point at A; discarding the orphan staging and reopening from A
  // recovers the pre-compaction layout.
  graph_->CompactVertexLabelGroups(person_label, 0.0);
  graph_->MarkVertexTableDirty(person_label);
  graph_->MarkEdgeTableDirty(person_label, person_label, knows_label);
  graph_->Compact();
  auto staging_b = checkpoint_mgr_.CreateStaging();
  graph_->DumpAndClear(staging_b.checkpoint());
  EXPECT_EQ(checkpoint_mgr_.Current()->id(), checkpoint_a->id())
      << "crash before publish must not advance CURRENT";
  staging_b.Discard();
  graph_->Open(checkpoint_mgr_.Current(), MemoryLevel::kInMemory);
  expect_original_layout();

  // Phase 2: remap + dump + publish, then recover the remapped layout from
  // disk through a fresh open.
  graph_->CompactVertexLabelGroups(person_label, 0.0);
  graph_->MarkVertexTableDirty(person_label);
  graph_->MarkEdgeTableDirty(person_label, person_label, knows_label);
  graph_->Compact();
  auto staging_c = checkpoint_mgr_.CreateStaging();
  graph_->DumpAndClear(staging_c.checkpoint());
  auto checkpoint_c = staging_c.Publish();
  EXPECT_EQ(checkpoint_mgr_.Current()->id(), checkpoint_c->id());
  graph_->Clear();
  graph_->Open(checkpoint_mgr_.Current(), MemoryLevel::kInMemory);
  for (size_t i = 0; i < survivors.size(); ++i) {
    vid_t vid = 0;
    ASSERT_TRUE(
        graph_->get_lid(person_label, Value::INT64(survivors[i]), vid, 3));
    EXPECT_EQ(vid, static_cast<vid_t>(i));
  }
  for (int64_t oid : {1, 3, 5}) {
    vid_t vid = 0;
    EXPECT_FALSE(graph_->get_lid(person_label, Value::INT64(oid), vid, 3));
  }
  // 6->7 is the only chain edge whose endpoints both survived the deletes.
  EXPECT_EQ(graph_->EdgeNum(person_label, knows_label, person_label), 1u);
}

}  // namespace neug
