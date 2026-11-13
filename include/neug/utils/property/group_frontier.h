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
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "neug/storages/chunk/row_group.h"
#include "neug/utils/property/types.h"

namespace neug {

/// Per-group append frontier for vid allocation (layout B).
///
/// vid = base_g + frontier_g. In the initial dense state frontier_g equals the
/// group's row count, so allocation appends to the active group's tail exactly
/// like the baseline global counter. After group-internal compaction the
/// frontier retreats to the valid-row count, so subsequent inserts refill the
/// contiguous tail holes in order (group-local offset stays monotonic).
///
/// Allocation is concurrency-safe (per-group atomic fetch_add). The frontier is
/// persisted with the checkpoint and restored on reopen. This is a logical
/// allocation cursor over the flat vid space, NOT a physical per-group
/// allocation unit (capacity semantics stay identical to the baseline).
class GroupFrontier {
 public:
  GroupFrontier() : layout_(kDefaultRowsPerGroup) {}
  explicit GroupFrontier(RowGroupLayout layout) : layout_(layout) {}

  GroupFrontier(const GroupFrontier&) = delete;
  GroupFrontier& operator=(const GroupFrontier&) = delete;

  /// Initialize frontiers for `group_count` groups (all zero). The backing
  /// slab is over-allocated (2x headroom, min 64 slots) so group extension on
  /// the insert path never reallocates while concurrent allocators scan it.
  void Init(size_t group_count) {
    const size_t slab = std::max(group_count * 2, size_t{64});
    frontiers_ = std::make_unique<std::atomic<uint32_t>[]>(slab);
    for (size_t g = 0; g < slab; ++g) {
      frontiers_[g].store(0, std::memory_order_relaxed);
    }
    slab_capacity_ = slab;
    group_count_.store(group_count, std::memory_order_relaxed);
    active_hint_.store(0, std::memory_order_relaxed);
  }

  /// Allocate the next vid in group `g` (concurrency-safe). Returns base_g +
  /// the previous frontier, then advances the frontier by one.
  vid_t Allocate(size_t g) {
    const uint32_t offset =
        frontiers_[g].fetch_add(1, std::memory_order_relaxed);
    return static_cast<vid_t>(layout_.GroupBegin(g) + offset);
  }

  /// Allocate the next vid from the lowest non-full group (concurrency-safe).
  /// Scanning starts at a monotonic hint: in the dense state the hint equals
  /// the active group and allocation matches the baseline global counter;
  /// after a retreat the hint rewinds so the compacted group's contiguous
  /// tail holes are refilled first. The per-group fetch_add may overshoot the
  /// capacity under contention; the loser retries in the next group, so no
  /// vid is ever handed out twice.
  vid_t AllocateNext() {
    const uint32_t cap = static_cast<uint32_t>(layout_.rows_per_group);
    size_t g = active_hint_.load(std::memory_order_relaxed);
    while (g < group_count_.load(std::memory_order_relaxed)) {
      const uint32_t offset =
          frontiers_[g].fetch_add(1, std::memory_order_relaxed);
      if (offset < cap) {
        return static_cast<vid_t>(layout_.GroupBegin(g) + offset);
      }
      // Overshot a full group: undo and move on.
      frontiers_[g].fetch_sub(1, std::memory_order_relaxed);
      ++g;
    }
    // Every existing group is full: open a fresh one.
    return Allocate(ExtendOneGroup());
  }

  /// Retreat group `g`'s frontier to `valid_count` after compaction, so the
  /// next insert refills the contiguous tail holes.
  void Retreat(size_t g, size_t valid_count) {
    frontiers_[g].store(static_cast<uint32_t>(valid_count),
                        std::memory_order_relaxed);
    // Rewind the allocation hint so the retreated group is refilled before
    // appending to later groups (monotonic CAS keeps concurrent Retreats
    // safe).
    size_t hint = active_hint_.load(std::memory_order_relaxed);
    while (hint > g && !active_hint_.compare_exchange_weak(
                           hint, g, std::memory_order_relaxed)) {}
  }

  /// Current frontier (next free offset) of group `g`.
  size_t Frontier(size_t g) const {
    return frontiers_[g].load(std::memory_order_relaxed);
  }

  /// Global vid where the next AllocateNext() would write: the lowest non-full
  /// group's base + frontier. Used on checkpoint reopen to prepare the column
  /// append area at the refill front (which a compaction may have retreated
  /// below the vid high-water), not at the high-water itself.
  size_t AppendBegin() const {
    const uint32_t cap = static_cast<uint32_t>(layout_.rows_per_group);
    const size_t count = group_count_.load(std::memory_order_relaxed);
    for (size_t g = 0; g < count; ++g) {
      const uint32_t f = frontiers_[g].load(std::memory_order_relaxed);
      if (f < cap) {
        return static_cast<size_t>(layout_.GroupBegin(g)) + f;
      }
    }
    // Every group is full: the next append opens a fresh group.
    return static_cast<size_t>(layout_.GroupBegin(count));
  }

  size_t group_count() const {
    return group_count_.load(std::memory_order_relaxed);
  }
  const RowGroupLayout& layout() const { return layout_; }

  /// Serialize frontiers for checkpoint persistence.
  std::vector<uint32_t> Save() const {
    const size_t count = group_count_.load(std::memory_order_relaxed);
    std::vector<uint32_t> out(count);
    for (size_t g = 0; g < count; ++g) {
      out[g] = frontiers_[g].load(std::memory_order_relaxed);
    }
    return out;
  }

  /// Restore frontiers from a checkpoint.
  void Load(const std::vector<uint32_t>& values) {
    Init(values.size());
    for (size_t g = 0; g < values.size(); ++g) {
      frontiers_[g].store(values[g], std::memory_order_relaxed);
    }
    // Resume allocation at the lowest non-full group.
    const uint32_t cap = static_cast<uint32_t>(layout_.rows_per_group);
    size_t hint = 0;
    while (hint < values.size() &&
           frontiers_[hint].load(std::memory_order_relaxed) >= cap) {
      ++hint;
    }
    active_hint_.store(hint, std::memory_order_relaxed);
  }

 private:
  /// Append one zero-initialized group; returns its id. Within the slab's
  /// headroom this is a lock-free count bump (spare slots are pre-zeroed at
  /// Init). Slab exhaustion coincides with indexer capacity exhaustion, so
  /// the regrow path runs where capacity growth is already serialized; it
  /// must not race concurrent allocators.
  size_t ExtendOneGroup() {
    std::lock_guard<std::mutex> lock(extend_mutex_);
    const size_t old_count = group_count_.load(std::memory_order_relaxed);
    if (old_count >= slab_capacity_) {
      const size_t slab = std::max(old_count * 2, size_t{64});
      auto grown = std::make_unique<std::atomic<uint32_t>[]>(slab);
      for (size_t g = 0; g < slab; ++g) {
        grown[g].store(
            g < old_count ? frontiers_[g].load(std::memory_order_relaxed) : 0,
            std::memory_order_relaxed);
      }
      frontiers_ = std::move(grown);
      slab_capacity_ = slab;
    }
    group_count_.store(old_count + 1, std::memory_order_relaxed);
    active_hint_.store(old_count, std::memory_order_relaxed);
    return old_count;
  }

  RowGroupLayout layout_;
  std::unique_ptr<std::atomic<uint32_t>[]> frontiers_;
  size_t slab_capacity_ = 0;
  std::atomic<size_t> group_count_{0};
  std::atomic<size_t> active_hint_{0};
  std::mutex extend_mutex_;
};

}  // namespace neug