// S3-FIFO, after Yang et al., "FIFO Queues are All You Need for Cache
// Eviction", SOSP 2023 (Spec §11.6). Queue budgets count bytes; ghosts count keys.
#include <algorithm>
#include <cstdint>
#include <list>
#include <memory>
#include <string_view>
#include <unordered_map>

#include "clache/policy.h"

#include "modern_queues.h"
namespace clache {
namespace {
class S3FifoPolicy final : public Policy {
 public:
  S3FifoPolicy(const PolicyConfig& c, const PolicyContext& x)
      : ctx_(x),
        q_(x.meta),
        threshold_(c.s3fifo.move_to_main_threshold),
        budget_(static_cast<std::uint64_t>(static_cast<long double>(x.capacity_bytes) *
                                           c.s3fifo.small_fraction)) {}
  std::string_view name() const override { return "s3fifo"; }
  void on_hit(SlotId s, TimePoint) override {
    auto& f = q_.nodes[s].freq;
    if (f < 3) ++f;
  }
  void on_insert(SlotId s, TimePoint) override {
    const auto it = ghost_index_.find((*ctx_.meta)[s].key);
    const bool ghost = it != ghost_index_.end();
    if (ghost) {
      ghosts_.erase(it->second);
      ghost_index_.erase(it);
    }
    q_.push(s, ghost ? 2 : 1);
    q_.nodes[s].freq = 0;
    trim();
  }
  void on_remove(SlotId s, RemovalReason) override {
    q_.unlink(s);
    if (s < q_.nodes.size()) q_.nodes[s] = {};
    trim();
  }
  void plan_admission(SlotId, std::uint64_t needed, TimePoint, AdmissionPlan& p) override {
    p.reset();
    p.admit = true;
    p.decision = Decision::AdmitWithEviction;
    while (p.victim_bytes < needed) {
      const bool small =
          q_.queues[1].count && (q_.queues[1].bytes >= budget_ || !q_.queues[2].count);
      SlotId s = q_.queues[small ? 1 : 2].tail;
      auto& f = q_.nodes[s].freq;
      if (small && f >= threshold_) {
        q_.push(s, 2);
        f = 0;
        continue;
      }
      if (!small && f > 0) {
        --f;
        q_.push(s, 2);
        continue;
      }
      q_.unlink(s);
      if (small) {
        const auto key = (*ctx_.meta)[s].key;
        ghosts_.push_front(key);
        ghost_index_[key] = ghosts_.begin();
      }
      trim();
      p.victims.push_back(s);
      p.victim_bytes += (*ctx_.meta)[s].charged_bytes;
    }
  }
  double value(SlotId s, TimePoint) const override {
    return s < q_.nodes.size() ? q_.nodes[s].freq : 0;
  }
  std::uint64_t metadata_bytes() const override {
    return q_.memory() + ghosts_.size() * (sizeof(KeyId) * 2 + sizeof(void*) * 5) +
           ghost_index_.bucket_count() * sizeof(void*);
  }
  bool check_invariants() const override {
    if (!q_.check() || ghosts_.size() != ghost_index_.size() ||
        ghosts_.size() > std::max<std::size_t>(1, q_.queues[2].count))
      return false;
    for (auto it = ghosts_.begin(); it != ghosts_.end(); ++it) {
      auto found = ghost_index_.find(*it);
      if (found == ghost_index_.end() || found->second != it) return false;
    }
    return true;
  }

 private:
  void trim() {
    while (ghosts_.size() > std::max<std::size_t>(1, q_.queues[2].count)) {
      ghost_index_.erase(ghosts_.back());
      ghosts_.pop_back();
    }
  }
  PolicyContext ctx_;
  modern::Queues q_;
  std::uint32_t threshold_;
  std::uint64_t budget_;
  std::list<KeyId> ghosts_;
  std::unordered_map<KeyId, std::list<KeyId>::iterator> ghost_index_;
};
}  // namespace
CLACHE_REGISTER_POLICY("s3fifo", [](const PolicyConfig& c, const PolicyContext& x) {
  return std::make_unique<S3FifoPolicy>(c, x);
});
}  // namespace clache
