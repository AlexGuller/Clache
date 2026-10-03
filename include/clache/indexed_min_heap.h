// Binary min-heap over SlotIds with O(1) position lookup (Spec §8).
//
// Order is lexicographic (key, seq, id) ascending, which is a total order as
// long as ids are unique. -inf keys are valid and sort first; NaN is a bug.
// Used for the Clache value index, GDS/GDSF/LFU-DA, and the engine's expiry
// index.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "clache/check.h"
#include "clache/types.h"

namespace clache {

class IndexedMinHeap {
 public:
  struct Node {
    double key;
    std::uint64_t seq;  // tie-break 1: older access first
    std::uint64_t id;   // tie-break 2: smaller KeyId first
    SlotId slot;
  };

  static bool less(const Node& a, const Node& b) noexcept {
    if (a.key < b.key) return true;
    if (b.key < a.key) return false;
    if (a.seq != b.seq) return a.seq < b.seq;
    return a.id < b.id;
  }

  void reserve(std::size_t n) {
    nodes_.reserve(n);
    pos_.reserve(n);
  }

  void clear() noexcept {
    for (const Node& n : nodes_) pos_[n.slot] = kAbsent;
    nodes_.clear();
  }

  bool empty() const noexcept { return nodes_.empty(); }
  std::size_t size() const noexcept { return nodes_.size(); }

  bool contains(SlotId slot) const noexcept { return slot < pos_.size() && pos_[slot] != kAbsent; }

  void push(SlotId slot, double key, std::uint64_t seq, std::uint64_t id) {
    CLACHE_ASSERT(!std::isnan(key), "heap key must not be NaN");
    CLACHE_ASSERT(!contains(slot), "slot already in heap");
    if (slot >= pos_.size()) pos_.resize(static_cast<std::size_t>(slot) + 1, kAbsent);
    nodes_.push_back(Node{key, seq, id, slot});
    const auto i = static_cast<std::uint32_t>(nodes_.size() - 1);
    pos_[slot] = i;
    sift_up(i);
  }

  // Changes key and seq of an existing entry; id is fixed for the slot's lifetime.
  void update(SlotId slot, double key, std::uint64_t seq) {
    CLACHE_ASSERT(!std::isnan(key), "heap key must not be NaN");
    CLACHE_ASSERT(contains(slot), "update of slot not in heap");
    const std::uint32_t i = pos_[slot];
    Node updated = nodes_[i];
    updated.key = key;
    updated.seq = seq;
    const bool moved_up = less(updated, nodes_[i]);
    nodes_[i] = updated;
    if (moved_up) {
      sift_up(i);
    } else {
      sift_down(i);
    }
  }

  void erase(SlotId slot) {
    CLACHE_ASSERT(contains(slot), "erase of slot not in heap");
    const std::uint32_t i = pos_[slot];
    const auto last = static_cast<std::uint32_t>(nodes_.size() - 1);
    pos_[slot] = kAbsent;
    if (i == last) {
      nodes_.pop_back();
      return;
    }
    nodes_[i] = nodes_[last];
    nodes_.pop_back();
    pos_[nodes_[i].slot] = i;
    if (i > 0 && less(nodes_[i], nodes_[parent(i)])) {
      sift_up(i);
    } else {
      sift_down(i);
    }
  }

  const Node& top() const {
    CLACHE_ASSERT(!nodes_.empty(), "top() of empty heap");
    return nodes_.front();
  }

  Node pop() {
    Node n = top();
    erase(n.slot);
    return n;
  }

  const Node& node(SlotId slot) const {
    CLACHE_ASSERT(contains(slot), "node() of slot not in heap");
    return nodes_[pos_[slot]];
  }

  // Bulk rescore: rewrite key/seq through this span (never slot or id), then
  // call heapify(). Used for the bucket-epoch rebuild (Spec §10.3).
  std::span<Node> mutable_nodes() noexcept { return nodes_; }
  std::span<const Node> nodes() const noexcept { return nodes_; }

  // O(n) bottom-up heap construction.
  void heapify() {
    for (std::uint32_t i = 0; i < nodes_.size(); ++i) pos_[nodes_[i].slot] = i;
    if (nodes_.size() < 2) return;
    for (auto i = static_cast<std::uint32_t>(nodes_.size() / 2); i-- > 0;) sift_down(i);
  }

  // Visits nodes in ascending order without mutating the heap: best-first
  // traversal with a frontier heap of indices. O(k log k) for k visited nodes;
  // steady-state calls do not allocate. `visit(const Node&)` returns false to stop.
  template <class Visitor>
  void for_each_ascending(Visitor&& visit) const {
    if (nodes_.empty()) return;
    auto& frontier = scratch_;
    frontier.clear();
    // std::*_heap builds a max-heap, so invert the comparison.
    const auto greater = [this](std::uint32_t a, std::uint32_t b) {
      return less(nodes_[b], nodes_[a]);
    };
    frontier.push_back(0);
    while (!frontier.empty()) {
      std::pop_heap(frontier.begin(), frontier.end(), greater);
      const std::uint32_t i = frontier.back();
      frontier.pop_back();
      if (!visit(static_cast<const Node&>(nodes_[i]))) return;
      const std::uint64_t left = 2 * static_cast<std::uint64_t>(i) + 1;
      for (std::uint64_t c = left; c <= left + 1 && c < nodes_.size(); ++c) {
        frontier.push_back(static_cast<std::uint32_t>(c));
        std::push_heap(frontier.begin(), frontier.end(), greater);
      }
    }
  }

  bool check_invariants() const {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
      const Node& n = nodes_[i];
      if (std::isnan(n.key)) return false;
      if (n.slot >= pos_.size() || pos_[n.slot] != i) return false;
      if (i > 0 && less(n, nodes_[(i - 1) / 2])) return false;
    }
    std::size_t present = 0;
    for (const std::uint32_t p : pos_) {
      if (p != kAbsent) ++present;
    }
    return present == nodes_.size();
  }

  std::uint64_t memory_bytes() const noexcept {
    return nodes_.capacity() * sizeof(Node) + pos_.capacity() * sizeof(std::uint32_t) +
           scratch_.capacity() * sizeof(std::uint32_t);
  }

 private:
  static constexpr std::uint32_t kAbsent = std::numeric_limits<std::uint32_t>::max();

  static constexpr std::uint32_t parent(std::uint32_t i) noexcept { return (i - 1) / 2; }

  void sift_up(std::uint32_t i) {
    Node moving = nodes_[i];
    while (i > 0) {
      const std::uint32_t p = parent(i);
      if (!less(moving, nodes_[p])) break;
      nodes_[i] = nodes_[p];
      pos_[nodes_[i].slot] = i;
      i = p;
    }
    nodes_[i] = moving;
    pos_[moving.slot] = i;
  }

  void sift_down(std::uint32_t i) {
    const auto n = static_cast<std::uint32_t>(nodes_.size());
    Node moving = nodes_[i];
    while (true) {
      const std::uint64_t left = 2 * static_cast<std::uint64_t>(i) + 1;
      if (left >= n) break;
      auto child = static_cast<std::uint32_t>(left);
      if (child + 1 < n && less(nodes_[child + 1], nodes_[child])) ++child;
      if (!less(nodes_[child], moving)) break;
      nodes_[i] = nodes_[child];
      pos_[nodes_[i].slot] = i;
      i = child;
    }
    nodes_[i] = moving;
    pos_[moving.slot] = i;
  }

  std::vector<Node> nodes_;
  std::vector<std::uint32_t> pos_;  // indexed by SlotId
  mutable std::vector<std::uint32_t> scratch_;
};

}  // namespace clache
