#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "clache/metadata.h"

namespace clache::modern {
// Slot-indexed intrusive queues. Detached victims remain safe to remove again.
class Queues {
 public:
  struct Node {
    SlotId prev = kNoSlot, next = kNoSlot;
    unsigned region = 0;
    std::uint8_t freq = 0;
  };
  struct Queue {
    SlotId head = kNoSlot, tail = kNoSlot;
    std::uint64_t bytes = 0;
    std::size_t count = 0;
  };
  explicit Queues(const MetadataStore* meta) : meta_(meta) {}
  void ensure(SlotId s) {
    if (s >= nodes.size()) nodes.resize(static_cast<std::size_t>(s) + 1);
  }
  void unlink(SlotId s) {
    if (s >= nodes.size() || nodes[s].region == 0) return;
    auto& n = nodes[s];
    auto& q = queues[n.region];
    if (n.prev != kNoSlot)
      nodes[n.prev].next = n.next;
    else
      q.head = n.next;
    if (n.next != kNoSlot)
      nodes[n.next].prev = n.prev;
    else
      q.tail = n.prev;
    q.bytes -= (*meta_)[s].charged_bytes;
    --q.count;
    n.prev = n.next = kNoSlot;
    n.region = 0;
  }
  void push(SlotId s, unsigned r) {
    ensure(s);
    unlink(s);
    auto& n = nodes[s];
    auto& q = queues[r];
    n.region = r;
    n.next = q.head;
    if (q.head != kNoSlot)
      nodes[q.head].prev = s;
    else
      q.tail = s;
    q.head = s;
    q.bytes += (*meta_)[s].charged_bytes;
    ++q.count;
  }
  bool check() const {
    std::size_t total = 0;
    for (unsigned r = 1; r < queues.size(); ++r) {
      const auto& q = queues[r];
      SlotId prev = kNoSlot;
      std::size_t count = 0;
      std::uint64_t bytes = 0;
      for (SlotId s = q.head; s != kNoSlot; s = nodes[s].next) {
        if (s >= nodes.size() || ++count > nodes.size()) return false;
        if (nodes[s].region != r || nodes[s].prev != prev || !(*meta_)[s].resident) return false;
        bytes += (*meta_)[s].charged_bytes;
        prev = s;
      }
      if (count != q.count || bytes != q.bytes || prev != q.tail) return false;
      total += count;
    }
    std::size_t active = 0;
    for (const auto& n : nodes)
      if (n.region) ++active;
    return active == total && total == meta_->resident_count();
  }
  std::uint64_t memory() const { return sizeof(*this) + nodes.capacity() * sizeof(Node); }
  std::vector<Node> nodes;
  std::array<Queue, 4> queues{};

 private:
  const MetadataStore* meta_;
};
}  // namespace clache::modern
