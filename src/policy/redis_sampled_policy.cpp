// Redis allkeys-lru / allkeys-lfu policy approximations, following Redis
// eviction documentation (Spec §11.4). These model sampling and logarithmic
// counters, not Redis memory accounting; real-Redis validation is Phase 4.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "clache/policy.h"
#include "clache/rng.h"
namespace clache {
namespace {
class RedisPolicy final : public Policy {
 public:
  RedisPolicy(const PolicyConfig& c, const PolicyContext& x, bool lfu)
      : cfg_(c.redis), ctx_(x), rng_(x.seed), lfu_(lfu) {
    if (!cfg_.samples || !cfg_.pool_size)
      throw std::invalid_argument("Redis sampling and pool sizes must be positive");
  }
  std::string_view name() const override { return lfu_ ? "redis_lfu" : "redis_lru"; }
  void on_access(SlotId s, TimePoint now) override {
    if (!lfu_ || s >= state_.size() || state_[s].position == kAbsent) return;
    auto& n = state_[s];
    if (cfg_.lfu_decay_time_min && now > n.decrement) {
      constexpr std::int64_t kMinuteNs = 60LL * 1'000'000'000;
      const auto elapsed_minutes = (now - n.decrement).count() / kMinuteNs;
      const auto periods = elapsed_minutes / cfg_.lfu_decay_time_min;
      n.counter = static_cast<std::uint8_t>(periods >= n.counter ? 0 : n.counter - periods);
      // Keep the fractional period, so frequent accesses cannot postpone decay.
      n.decrement = n.decrement + Nanos(periods * cfg_.lfu_decay_time_min * kMinuteNs);
    }
    const auto base = n.counter > 5 ? n.counter - 5 : 0;
    const double probability = 1.0 / (static_cast<double>(base) * cfg_.lfu_log_factor + 1.0);
    if (n.counter < 255 && rng_.bernoulli(probability)) ++n.counter;
  }
  void on_hit(SlotId, TimePoint) override {}
  void on_insert(SlotId s, TimePoint now) override {
    if (s >= state_.size()) state_.resize(static_cast<std::size_t>(s) + 1);
    state_[s] = {residents_.size(), 5, now};
    residents_.push_back(s);
  }
  void on_remove(SlotId s, RemovalReason) override {
    detach(s);
    if (s < state_.size()) state_[s] = {};
    std::erase_if(pool_, [s](const Pool& p) { return p.slot == s; });
  }
  void plan_admission(SlotId, std::uint64_t needed, TimePoint now, AdmissionPlan& p) override {
    p.reset();
    p.admit = true;
    p.decision = Decision::AdmitWithEviction;
    while (p.victim_bytes < needed) {
      for (std::uint32_t i = 0; i < cfg_.samples; ++i) {
        const SlotId s = residents_[rng_.uniform_below(residents_.size())];
        std::erase_if(pool_, [s](const Pool& x) { return x.slot == s; });
        pool_.push_back({s, score(s, now), (*ctx_.meta)[s].last_access_seq, (*ctx_.meta)[s].key});
        std::sort(pool_.begin(), pool_.end(), [](const Pool& a, const Pool& b) {
          if (a.score != b.score) return a.score > b.score;
          if (a.seq != b.seq) return a.seq < b.seq;
          return a.key < b.key;
        });
        if (pool_.size() > cfg_.pool_size) pool_.pop_back();
      }
      const auto s = pool_.front().slot;
      pool_.erase(pool_.begin());
      // Remove planned victims from the sampling population immediately.
      detach(s);
      p.victims.push_back(s);
      p.victim_bytes += (*ctx_.meta)[s].charged_bytes;
    }
  }
  double value(SlotId s, TimePoint now) const override {
    return static_cast<double>(score(s, now));
  }
  std::uint64_t metadata_bytes() const override {
    return sizeof(*this) + state_.capacity() * sizeof(State) +
           residents_.capacity() * sizeof(SlotId) + pool_.capacity() * sizeof(Pool);
  }
  bool check_invariants() const override {
    for (std::size_t i = 0; i < residents_.size(); ++i) {
      auto s = residents_[i];
      if (s >= state_.size() || state_[s].position != i || !(*ctx_.meta)[s].resident) return false;
    }
    std::size_t n = 0;
    for (const auto& s : state_)
      if (s.position != kAbsent) ++n;
    if (n != residents_.size() || n != ctx_.meta->resident_count() || pool_.size() > cfg_.pool_size)
      return false;
    for (std::size_t i = 0; i < pool_.size(); ++i) {
      if (state_[pool_[i].slot].position == kAbsent) return false;
      for (std::size_t j = 0; j < i; ++j)
        if (pool_[j].slot == pool_[i].slot) return false;
    }
    return true;
  }

 private:
  static constexpr auto kAbsent = std::numeric_limits<std::size_t>::max();
  struct State {
    std::size_t position = kAbsent;
    std::uint8_t counter = 0;
    TimePoint decrement{};
  };
  struct Pool {
    SlotId slot;
    std::uint64_t score, seq;
    KeyId key;
  };
  std::uint64_t score(SlotId s, TimePoint now) const {
    if (lfu_) return s < state_.size() ? 255U - state_[s].counter : 255U;
    const auto last = (*ctx_.meta)[s].last_access;
    return now > last ? static_cast<std::uint64_t>((now - last).count()) : 0;
  }
  void detach(SlotId s) {
    if (s >= state_.size() || state_[s].position == kAbsent) return;
    const auto pos = state_[s].position;
    residents_[pos] = residents_.back();
    state_[residents_.back()].position = pos;
    residents_.pop_back();
    state_[s].position = kAbsent;
  }
  RedisPolicyConfig cfg_;
  PolicyContext ctx_;
  Xoshiro256ss rng_;
  bool lfu_;
  std::vector<State> state_;
  std::vector<SlotId> residents_;
  std::vector<Pool> pool_;
};
}  // namespace
CLACHE_REGISTER_POLICY("redis_lru", [](const PolicyConfig& c, const PolicyContext& x) {
  return std::make_unique<RedisPolicy>(c, x, false);
});
CLACHE_REGISTER_POLICY("redis_lfu", [](const PolicyConfig& c, const PolicyContext& x) {
  return std::make_unique<RedisPolicy>(c, x, true);
});
}  // namespace clache
