#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "clache/check.h"
#include "clache/indexed_min_heap.h"
#include "clache/policy.h"

namespace clache {
namespace {
class CostValuePolicy final : public Policy {
 public:
  CostValuePolicy(const PolicyConfig& config, const PolicyContext& context, std::string_view name)
      : context_(context), config_(config.clache), name_(name) {
    if (!context.meta || !context.freq || !context.cost || !context.engine) {
      throw std::invalid_argument("Clache requires metadata, estimators and engine context");
    }
    exponent_ = name == "clache_nosize" ? 0.0 : context.engine->value.size_exponent;
    if (name == "clache_noadmit")
      config_.admission_rule = ClachePolicyConfig::AdmissionRule::Always;
    if (name == "clache_nocost") config_.use_cost = false;
  }

  std::string_view name() const override { return name_; }

  void on_access(SlotId, TimePoint now) override {
    now_ = now;
    if (context_.freq->mode() != FrequencyEstimator::Mode::RollingBuckets) return;
    const auto epoch = context_.freq->epoch(now);
    if (epoch == rebuilt_epoch_) return;
    for (auto& node : heap_.mutable_nodes()) node.key = score(node.slot, now);
    heap_.heapify();
    rebuilt_epoch_ = epoch;
  }

  void on_hit(SlotId slot, TimePoint now) override { update(slot, now); }
  void on_cost_update(SlotId slot, TimePoint now) override {
    on_access(slot, now);
    update(slot, now);
  }
  void on_insert(SlotId slot, TimePoint now) override {
    on_access(slot, now);
    const auto& entry = (*context_.meta)[slot];
    heap_.push(slot, score(slot, now), entry.last_access_seq, entry.key);
  }
  void on_remove(SlotId slot, RemovalReason) override { heap_.erase(slot); }

  void plan_admission(SlotId candidate, std::uint64_t bytes_needed, TimePoint now,
                      AdmissionPlan& plan) override {
    plan.reset();
    const double candidate_key = score(candidate, now);
    plan.candidate_value = from_key(candidate_key, now);
    plan.decision = Decision::RejectCandidateNotHigher;
    std::uint64_t bytes = 0;
    heap_.for_each_ascending([&](const IndexedMinHeap::Node& node) {
      plan.threshold_value = from_key(node.key, now);
      if (config_.admission_rule == ClachePolicyConfig::AdmissionRule::Strict &&
          node.key >= candidate_key)
        return false;
      if (context_.engine->admission.max_victims > 0 &&
          plan.victims.size() == context_.engine->admission.max_victims) {
        plan.decision = Decision::RejectTooManyVictims;
        return false;
      }
      plan.victims.push_back(node.slot);
      bytes += (*context_.meta)[node.slot].charged_bytes;
      return bytes < bytes_needed;
    });
    if (bytes >= bytes_needed) {
      plan.admit = true;
      plan.decision = Decision::AdmitWithEviction;
      plan.victim_bytes = bytes;
    } else {
      plan.victims.clear();
    }
  }

  double value(SlotId slot, TimePoint now) const override {
    return from_key(score(slot, now), now);
  }
  std::uint64_t metadata_bytes() const override { return heap_.memory_bytes(); }
  bool check_invariants() const override {
    if (!heap_.check_invariants() || heap_.size() != context_.meta->resident_count()) return false;
    for (const auto& node : heap_.nodes()) {
      const auto& entry = (*context_.meta)[node.slot];
      if (!entry.resident || node.id != entry.key || node.seq != entry.last_access_seq ||
          node.key != score(node.slot, now_))
        return false;
    }
    return true;
  }

 private:
  double score(SlotId slot, TimePoint now) const {
    const auto& entry = (*context_.meta)[slot];
    return context_.freq->log_rate_key(entry.freq, now) +
           (config_.use_cost ? std::log(context_.cost->estimate_ms(entry.cost)) : 0.0) -
           exponent_ *
               std::log(static_cast<double>(std::max(std::uint64_t{1}, entry.charged_bytes)));
  }
  double from_key(double key, TimePoint now) const {
    return std::exp(key - context_.freq->time_offset(now));
  }
  void update(SlotId slot, TimePoint now) {
    now_ = now;
    heap_.update(slot, score(slot, now), (*context_.meta)[slot].last_access_seq);
  }
  PolicyContext context_;
  ClachePolicyConfig config_;
  std::string_view name_;
  double exponent_ = 1.0;
  TimePoint now_{};
  std::uint32_t rebuilt_epoch_ = 0;
  IndexedMinHeap heap_;
};
}  // namespace

#define CLACHE_VALUE_VARIANT(name)                                                            \
  CLACHE_REGISTER_POLICY(name, [](const PolicyConfig& config, const PolicyContext& context) { \
    return std::make_unique<CostValuePolicy>(config, context, name);                          \
  });
CLACHE_VALUE_VARIANT("clache")
CLACHE_VALUE_VARIANT("clache_noadmit")
CLACHE_VALUE_VARIANT("clache_nocost")
CLACHE_VALUE_VARIANT("clache_nosize")
CLACHE_VALUE_VARIANT("clache_decay")
CLACHE_VALUE_VARIANT("clache_noghost")
#undef CLACHE_VALUE_VARIANT
}  // namespace clache
