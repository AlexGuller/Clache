// LFU dynamic aging: Arlitt et al., "Evaluating Content Management
// Techniques for Web Proxy Caches" (1999). GreedyDual-Size: Cao and Irani,
// "Cost-Aware WWW Proxy Caching Algorithms" (1997); GDSF: Cherkasova (1998).
// Scores are refreshed on insertion, hits and cost updates, per Spec §11.3.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

#include "clache/check.h"
#include "clache/indexed_min_heap.h"
#include "clache/policy.h"

namespace clache::detail {

class ClassicHeapPolicy final : public Policy {
 public:
  enum class Kind { Lfu, LfuDa, Gds, Gdsf, GdsfWindow };

  ClassicHeapPolicy(Kind kind, const PolicyConfig& config, const PolicyContext& context)
      : kind_(kind), config_(config.gdsf), ctx_(context) {
    CLACHE_REQUIRE(ctx_.meta != nullptr, "classic policy requires metadata");
    if (is_gds()) {
      CLACHE_REQUIRE(ctx_.cost != nullptr, "GDS requires a cost estimator");
      CLACHE_REQUIRE(std::isfinite(config_.size_exponent) && config_.size_exponent >= 0 &&
                         config_.size_exponent <= 2,
                     "GDS size exponent must be in [0, 2]");
    }
    if (window_frequency()) {
      CLACHE_REQUIRE(ctx_.freq != nullptr, "GDSF window requires a frequency estimator");
    }
  }

  std::string_view name() const override {
    switch (kind_) {
      case Kind::Lfu:
        return "lfu";
      case Kind::LfuDa:
        return "lfu_da";
      case Kind::Gds:
        return "gds";
      case Kind::Gdsf:
        return "gdsf";
      case Kind::GdsfWindow:
        return "gdsf_window";
    }
    return "";
  }

  void on_insert(SlotId slot, TimePoint now) override {
    if (slot >= counts_.size()) counts_.resize(static_cast<std::size_t>(slot) + 1);
    counts_[slot] = 1;
    const auto& m = (*ctx_.meta)[slot];
    heap_.push(slot, score(slot, now, 1), m.last_access_seq, m.key);
    ++resident_count_;
  }

  void on_hit(SlotId slot, TimePoint now) override {
    if (counts_[slot] != std::numeric_limits<std::uint64_t>::max()) ++counts_[slot];
    refresh(slot, now);
  }

  void on_cost_update(SlotId slot, TimePoint now) override {
    if (is_gds()) refresh(slot, now);
  }

  void on_remove(SlotId slot, RemovalReason reason) override {
    if (kind_ != Kind::Lfu && reason == RemovalReason::Evicted) {
      age_ = std::max(age_, heap_.node(slot).key);
    }
    heap_.erase(slot);
    counts_[slot] = 0;
    --resident_count_;
  }

  void plan_admission(SlotId candidate, std::uint64_t bytes_needed, TimePoint now,
                      AdmissionPlan& plan) override {
    plan.reset();
    plan.candidate_value = score(candidate, now, 1);
    heap_.for_each_ascending([&](const IndexedMinHeap::Node& n) {
      plan.victims.push_back(n.slot);
      plan.victim_bytes += (*ctx_.meta)[n.slot].charged_bytes;
      plan.threshold_value = n.key;
      return plan.victim_bytes < bytes_needed;
    });
    CLACHE_REQUIRE(plan.victim_bytes >= bytes_needed, "insufficient resident bytes for admission");
    plan.admit = true;
    plan.decision = Decision::AdmitWithEviction;
  }

  double value(SlotId slot, TimePoint now) const override {
    return heap_.contains(slot) ? heap_.node(slot).key : score(slot, now, 1);
  }

  std::uint64_t metadata_bytes() const override {
    return heap_.memory_bytes() + counts_.capacity() * sizeof(std::uint64_t);
  }

  bool check_invariants() const override {
    if (!heap_.check_invariants() || heap_.size() != resident_count_) return false;
    std::size_t count = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
      if (counts_[i] == 0) continue;
      ++count;
      if (!heap_.contains(static_cast<SlotId>(i))) return false;
    }
    for (const auto& n : heap_.nodes()) {
      const auto& m = (*ctx_.meta)[n.slot];
      if (!m.resident || m.key != n.id || counts_[n.slot] == 0) return false;
    }
    return count == resident_count_;
  }

 private:
  bool is_gds() const { return kind_ != Kind::Lfu && kind_ != Kind::LfuDa; }
  bool window_frequency() const {
    return kind_ == Kind::GdsfWindow ||
           (kind_ == Kind::Gdsf &&
            config_.frequency_source == GdsfPolicyConfig::FrequencySource::EngineWindow);
  }
  double score(SlotId slot, TimePoint now, std::uint64_t count) const {
    if (!is_gds()) return age_ + static_cast<double>(count);
    const auto& m = (*ctx_.meta)[slot];
    const double frequency = kind_ == Kind::Gds ? 1.0
                                                : (window_frequency() ? ctx_.freq->rate(m.freq, now)
                                                                      : static_cast<double>(count));
    return age_ + frequency * ctx_.cost->estimate_ms(m.cost) /
                      std::pow(static_cast<double>(std::max<std::uint64_t>(1, m.charged_bytes)),
                               config_.size_exponent);
  }
  void refresh(SlotId slot, TimePoint now) {
    heap_.update(slot, score(slot, now, counts_[slot]), (*ctx_.meta)[slot].last_access_seq);
  }

  Kind kind_;
  GdsfPolicyConfig config_;
  PolicyContext ctx_;
  IndexedMinHeap heap_;
  std::vector<std::uint64_t> counts_;
  std::size_t resident_count_ = 0;
  double age_ = 0;
};
}  // namespace clache::detail
