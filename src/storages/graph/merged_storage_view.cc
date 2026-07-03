#include "neug/storages/graph/merged_storage_view.h"

#include <cstring>
#include <limits>

namespace neug {

LabelOffsetTable::LabelOffsetTable(const StorageReadInterface& graph,
                                   const std::vector<label_t>& vertex_labels) {
  ranges_.reserve(vertex_labels.size());
  vid_t offset = 0;
  for (auto label : vertex_labels) {
    vid_t cap = graph.GetVertexSet(label).size();
    ranges_.push_back({label, offset, cap});
    offset += cap;
  }
  total_capacity_ = offset;
}

std::pair<size_t, vid_t> LabelOffsetTable::global_to_local(
    vid_t global_vid) const {
  for (size_t i = ranges_.size(); i > 0; --i) {
    size_t idx = i - 1;
    if (global_vid >= ranges_[idx].offset) {
      return {idx, global_vid - ranges_[idx].offset};
    }
  }
  return {0, global_vid};
}

size_t LabelOffsetTable::find_label_idx(label_t label) const {
  for (size_t i = 0; i < ranges_.size(); ++i) {
    if (ranges_[i].label == label) return i;
  }
  return SIZE_MAX;
}

MergedStorageView::MergedStorageView(
    const StorageReadInterface& base_graph,
    const std::vector<label_t>& vertex_labels,
    const std::vector<execution::LabelTriplet>& edge_triplets)
    : StorageReadInterface(base_graph.view(), base_graph.read_ts()),
      offset_table_(base_graph, vertex_labels) {
  vid_t total_cap = offset_table_.total_capacity();
  merged_vts_.Init(0, total_cap);
  for (size_t li = 0; li < vertex_labels.size(); ++li) {
    auto vs = base_graph.GetVertexSet(vertex_labels[li]);
    vid_t off = offset_table_.range(li).offset;
    for (vid_t v : vs) {
      merged_vts_.InsertVertex(off + v, 0);
    }
  }
  build_adjacency(base_graph, edge_triplets);
}

void MergedStorageView::build_adjacency(
    const StorageReadInterface& base_graph,
    const std::vector<execution::LabelTriplet>& edge_triplets) {
  vid_t total_cap = offset_table_.total_capacity();
  if (total_cap == 0) return;

  degrees_out_ = std::make_unique<int[]>(total_cap);
  degrees_in_ = std::make_unique<int[]>(total_cap);
  std::memset(degrees_out_.get(), 0, sizeof(int) * total_cap);
  std::memset(degrees_in_.get(), 0, sizeof(int) * total_cap);

  for (const auto& triplet : edge_triplets) {
    size_t src_idx = offset_table_.find_label_idx(triplet.src_label);
    size_t dst_idx = offset_table_.find_label_idx(triplet.dst_label);
    if (src_idx == SIZE_MAX || dst_idx == SIZE_MAX) continue;
    vid_t src_off = offset_table_.range(src_idx).offset;
    vid_t dst_off = offset_table_.range(dst_idx).offset;

    auto oe_view = base_graph.GetGenericOutgoingGraphView(
        triplet.src_label, triplet.dst_label, triplet.edge_label);
    auto vs = base_graph.GetVertexSet(triplet.src_label);
    for (vid_t v : vs) {
      int count = 0;
      auto edges = oe_view.get_edges(v);
      for (auto it = edges.begin(); it != edges.end(); ++it) ++count;
      degrees_out_[src_off + v] += count;
    }

    auto ie_view = base_graph.GetGenericIncomingGraphView(
        triplet.dst_label, triplet.src_label, triplet.edge_label);
    auto vs_dst = base_graph.GetVertexSet(triplet.dst_label);
    for (vid_t v : vs_dst) {
      int count = 0;
      auto edges = ie_view.get_edges(v);
      for (auto it = edges.begin(); it != edges.end(); ++it) ++count;
      degrees_in_[dst_off + v] += count;
    }
  }

  size_t total_out = 0, total_in = 0;
  for (vid_t i = 0; i < total_cap; ++i) {
    total_out += degrees_out_[i];
    total_in += degrees_in_[i];
  }

  adjlist_ptrs_out_ = std::make_unique<int64_t[]>(total_cap);
  adjlist_ptrs_in_ = std::make_unique<int64_t[]>(total_cap);
  if (total_out > 0) nbr_buf_out_ = std::make_unique<vid_t[]>(total_out);
  if (total_in > 0) nbr_buf_in_ = std::make_unique<vid_t[]>(total_in);

  {
    vid_t* buf = nbr_buf_out_.get();
    for (vid_t i = 0; i < total_cap; ++i) {
      adjlist_ptrs_out_[i] = reinterpret_cast<int64_t>(buf);
      buf += degrees_out_[i];
    }
  }
  {
    vid_t* buf = nbr_buf_in_.get();
    for (vid_t i = 0; i < total_cap; ++i) {
      adjlist_ptrs_in_[i] = reinterpret_cast<int64_t>(buf);
      buf += degrees_in_[i];
    }
  }

  auto cursor_out = std::make_unique<int[]>(total_cap);
  auto cursor_in = std::make_unique<int[]>(total_cap);
  std::memset(cursor_out.get(), 0, sizeof(int) * total_cap);
  std::memset(cursor_in.get(), 0, sizeof(int) * total_cap);

  for (const auto& triplet : edge_triplets) {
    size_t src_idx = offset_table_.find_label_idx(triplet.src_label);
    size_t dst_idx = offset_table_.find_label_idx(triplet.dst_label);
    if (src_idx == SIZE_MAX || dst_idx == SIZE_MAX) continue;
    vid_t src_off = offset_table_.range(src_idx).offset;
    vid_t dst_off = offset_table_.range(dst_idx).offset;

    auto oe_view = base_graph.GetGenericOutgoingGraphView(
        triplet.src_label, triplet.dst_label, triplet.edge_label);
    auto vs_src = base_graph.GetVertexSet(triplet.src_label);
    for (vid_t v : vs_src) {
      vid_t gv = src_off + v;
      vid_t* base_ptr = reinterpret_cast<vid_t*>(adjlist_ptrs_out_[gv]);
      auto edges = oe_view.get_edges(v);
      for (auto it = edges.begin(); it != edges.end(); ++it) {
        base_ptr[cursor_out[gv]++] = dst_off + (*it);
      }
    }

    auto ie_view = base_graph.GetGenericIncomingGraphView(
        triplet.dst_label, triplet.src_label, triplet.edge_label);
    auto vs_dst = base_graph.GetVertexSet(triplet.dst_label);
    for (vid_t v : vs_dst) {
      vid_t gv = dst_off + v;
      vid_t* base_ptr = reinterpret_cast<vid_t*>(adjlist_ptrs_in_[gv]);
      auto edges = ie_view.get_edges(v);
      for (auto it = edges.begin(); it != edges.end(); ++it) {
        base_ptr[cursor_in[gv]++] = src_off + (*it);
      }
    }
  }

  NbrIterConfig cfg;
  cfg.stride = sizeof(vid_t);
  cfg.ts_offset = 0;
  cfg.data_offset = 0;
  timestamp_t max_ts = std::numeric_limits<timestamp_t>::max();
  out_view_ = CsrView(reinterpret_cast<const char*>(adjlist_ptrs_out_.get()),
                       degrees_out_.get(), cfg, max_ts, 0);
  in_view_ = CsrView(reinterpret_cast<const char*>(adjlist_ptrs_in_.get()),
                      degrees_in_.get(), cfg, max_ts, 0);
}

VertexSet MergedStorageView::GetVertexSet(label_t) const {
  return VertexSet(offset_table_.total_capacity(), merged_vts_, 0);
}

CsrView MergedStorageView::GetGenericOutgoingGraphView(label_t, label_t,
                                                       label_t) const {
  return out_view_;
}

CsrView MergedStorageView::GetGenericIncomingGraphView(label_t, label_t,
                                                       label_t) const {
  return in_view_;
}

}  // namespace neug
