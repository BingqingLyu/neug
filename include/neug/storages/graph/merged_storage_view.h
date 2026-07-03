#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "neug/execution/common/types/graph_types.h"
#include "neug/storages/graph/graph_interface.h"
#include "neug/storages/graph/vertex_timestamp.h"

namespace neug {

struct LabelRange {
  label_t label;
  vid_t offset;
  vid_t capacity;
};

class LabelOffsetTable {
 public:
  LabelOffsetTable() = default;
  LabelOffsetTable(const StorageReadInterface& graph,
                   const std::vector<label_t>& vertex_labels);

  vid_t local_to_global(size_t label_idx, vid_t local_vid) const {
    return ranges_[label_idx].offset + local_vid;
  }
  std::pair<size_t, vid_t> global_to_local(vid_t global_vid) const;
  vid_t total_capacity() const { return total_capacity_; }
  size_t label_count() const { return ranges_.size(); }
  const LabelRange& range(size_t idx) const { return ranges_[idx]; }
  size_t find_label_idx(label_t label) const;

 private:
  std::vector<LabelRange> ranges_;
  vid_t total_capacity_ = 0;
};

class MergedStorageView : public StorageReadInterface {
 public:
  MergedStorageView(const StorageReadInterface& base_graph,
                    const std::vector<label_t>& vertex_labels,
                    const std::vector<execution::LabelTriplet>& edge_triplets);
  ~MergedStorageView() override = default;

  VertexSet GetVertexSet(label_t label) const override;
  CsrView GetGenericOutgoingGraphView(label_t v_label, label_t neighbor_label,
                                      label_t edge_label) const override;
  CsrView GetGenericIncomingGraphView(label_t v_label, label_t neighbor_label,
                                      label_t edge_label) const override;

  const LabelOffsetTable& offset_table() const { return offset_table_; }

 private:
  void build_adjacency(const StorageReadInterface& base_graph,
                       const std::vector<execution::LabelTriplet>& edge_triplets);

  LabelOffsetTable offset_table_;
  VertexTimestamp merged_vts_;
  std::unique_ptr<int[]> degrees_out_;
  std::unique_ptr<int64_t[]> adjlist_ptrs_out_;
  std::unique_ptr<vid_t[]> nbr_buf_out_;
  std::unique_ptr<int[]> degrees_in_;
  std::unique_ptr<int64_t[]> adjlist_ptrs_in_;
  std::unique_ptr<vid_t[]> nbr_buf_in_;
  CsrView out_view_;
  CsrView in_view_;
};

}  // namespace neug
