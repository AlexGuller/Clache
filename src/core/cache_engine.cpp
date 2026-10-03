#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_set>

#include "clache/check.h"
#include "clache/engine.h"

namespace clache {
namespace {
EngineConfig normalized(EngineConfig config, std::string_view name) {
  if (name == "clache_decay") config.frequency.mode = FrequencyConfig::Mode::ForwardDecay;
  if (name == "clache_noghost") config.observation.max_entries = 0;
  const auto errors = validate(config);
  if (!errors.empty()) throw std::invalid_argument(errors.front());
  return config;
}
}  // namespace
struct CacheEngine::Impl {
  EngineConfig config;
  MetadataStore meta;
  FrequencyEstimator freq;
  MissCostEstimator cost;
  std::unique_ptr<Policy> policy;
  DecisionSink* sink;
  EngineStats stats;
  std::uint64_t bytes = 0;
  TimePoint last{};
  TimePoint last_purge{};
  bool started = false;
  std::map<std::pair<TimePoint, SlotId>, KeyId> expiry;
  std::set<SlotId> residents;
  std::vector<KeyId> expired, removed;
  AdmissionPlan plan;
  Impl(EngineConfig c, std::string_view name, const PolicyConfig& pc, DecisionSink* ds)
      : config(normalized(c, name)),
        meta(config.observation),
        freq(config.frequency),
        cost(config.miss_cost),
        sink(ds) {
    const auto errors = validate(pc);
    if (!errors.empty()) throw std::invalid_argument(errors.front());
    PolicyContext context{&meta, &freq, &cost, &config, config.capacity_bytes, config.seed, 0};
    policy = make_policy(name, pc, context);
  }
  void begin(TimePoint now, bool request = false) {
    if (started && now < last) throw std::invalid_argument("engine time moved backwards");
    if (!started) {
      freq.set_origin(now);
      last = now;
      started = true;
    }
    if (started && config.frequency.mode == FrequencyConfig::Mode::RollingBuckets &&
        freq.epoch(now) != freq.epoch(last) && policy->name().starts_with("clache")) {
      ++stats.heap_rebuilds;
    }
    last = now;
    if (!request && policy->name().starts_with("clache")) policy->on_access(kNoSlot, now);
    expired.clear();
    removed.clear();
  }
  void remove(SlotId slot, RemovalReason reason, TimePoint now) {
    auto& m = meta[slot];
    policy->on_remove(slot, reason);
    expiry.erase({m.expiry, slot});
    bytes -= m.charged_bytes;
    residents.erase(slot);
    meta.mark_nonresident(slot, now);
    m.expiry = kNever;
    switch (reason) {
      case RemovalReason::Evicted:
        ++stats.evictions;
        stats.bytes_evicted += m.charged_bytes;
        break;
      case RemovalReason::Expired:
        ++stats.expirations;
        break;
      case RemovalReason::Invalidated:
        ++stats.invalidations;
        break;
      case RemovalReason::Replaced:
        ++stats.size_changes;
        break;
    }
  }
  void purge(TimePoint now) {
    last_purge = now;
    while (!expiry.empty() && expiry.begin()->first.first <= now) {
      SlotId slot = expiry.begin()->first.second;
      expired.push_back(meta[slot].key);
      remove(slot, RemovalReason::Expired, now);
    }
  }
  bool invariants(TimePoint now) const {
    std::uint64_t sum = 0;
    std::size_t ttl_count = 0;
    for (SlotId slot : residents) {
      const auto& m = meta[slot];
      if (!m.resident || m.charged_bytes > config.capacity_bytes - sum) return false;
      sum += m.charged_bytes;
      if (m.expiry != kNever) {
        ++ttl_count;
        if (m.expiry <= now || !expiry.contains({m.expiry, slot})) return false;
      }
    }
    return sum == bytes && residents.size() == meta.resident_count() &&
           ttl_count == expiry.size() && meta.check_invariants() && policy->check_invariants();
  }
  void finish(TimePoint now) {
    meta.trim();
    (void)now;
    CLACHE_ASSERT(invariants(last_purge), "cache engine invariants");
  }
  void record(SlotId slot, Decision decision, TimePoint now, std::uint64_t needed = 0) {
    ++stats.by_decision[static_cast<std::size_t>(decision)];
    if (!sink || (decision == Decision::Hit && !config.telemetry.sample_hits) ||
        !should_sample(meta[slot].last_access_seq, config.seed,
                       config.telemetry.decision_sample_rate))
      return;
    const auto& m = meta[slot];
    DecisionRecord r;
    r.seq = m.last_access_seq;
    r.now = now;
    r.key = m.key;
    r.decision = decision;
    r.policy = policy->name();
    r.candidate_value = plan.candidate_value;
    r.threshold_value = plan.threshold_value;
    r.frequency_rps = freq.rate(m.freq, now);
    r.miss_cost_ms = cost.estimate_ms(m.cost);
    r.size_bytes = m.charged_bytes;
    r.bytes_needed = needed;
    r.victim_count = static_cast<std::uint32_t>(removed.size());
    std::copy_n(removed.begin(), std::min(removed.size(), r.victims_head.size()),
                r.victims_head.begin());
    sink->record(r);
  }
};
CacheEngine::CacheEngine(EngineConfig config, std::string_view name, const PolicyConfig& pc,
                         DecisionSink* sink)
    : impl_(std::make_unique<Impl>(config, name, pc, sink)) {}
CacheEngine::~CacheEngine() = default;
LookupResult CacheEngine::lookup(KeyId key, TimePoint now, std::optional<std::uint64_t> expected) {
  auto& p = *impl_;
  p.begin(now, true);
  p.purge(now);
  p.plan.reset();
  SlotId slot = p.meta.lookup_or_observe(key, now);
  auto& m = p.meta[slot];
  ++p.stats.requests;
  if (p.freq.record(m.freq, now)) ++p.stats.freq_saturations;
  m.last_access = now;
  m.last_access_seq = p.stats.requests;
  if (m.access_count != std::numeric_limits<std::uint32_t>::max()) ++m.access_count;
  p.policy->on_access(slot, now);
  if (m.resident && expected && *expected != m.charged_bytes) {
    p.removed.push_back(key);
    p.remove(slot, RemovalReason::Replaced, now);
  }
  const bool hit = m.resident;
  if (hit) {
    ++p.stats.hits;
    p.stats.origin_cost_ms_saved_estimate += p.cost.estimate_ms(m.cost);
    p.policy->on_hit(slot, now);
    ++p.stats.score_updates;
    p.record(slot, Decision::Hit, now);
  } else {
    ++p.stats.misses;
    p.meta.pin(slot);
  }
  auto generation = m.generation;
  p.finish(now);
  return {hit, slot, generation, p.expired, p.removed};
}
AdmissionResult CacheEngine::complete_miss(KeyId key, std::uint64_t size, double cost_ms,
                                           std::optional<Nanos> ttl, std::uint32_t generation,
                                           TimePoint now) {
  auto& p = *impl_;
  p.begin(now);
  p.purge(now);
  p.plan.reset();
  SlotId slot = p.meta.find(key);
  CLACHE_REQUIRE(slot != kNoSlot && p.meta[slot].pins > 0,
                 "complete_miss requires a missing lookup");
  auto& m = p.meta[slot];
  Decision decision = Decision::RejectPolicy;
  std::uint64_t needed = 0;
  if (!m.resident) {
    m.charged_bytes = size;
    if (std::isfinite(cost_ms) && cost_ms >= 0) {
      p.cost.observe(m.cost, cost_ms);
      p.stats.origin_cost_ms_total += cost_ms;
    }
    if (generation != m.generation)
      decision = Decision::RejectInvalidatedDuringLoad;
    else if (size > p.config.capacity_bytes ||
             (p.config.max_object_bytes && size > p.config.max_object_bytes))
      decision = Decision::RejectOversized;
    else if (m.access_count < p.config.admission.min_observations)
      decision = Decision::RejectInsufficientHistory;
    else if (ttl && ttl->count() <= 0)
      decision = Decision::RejectPolicy;
    else {
      auto free = p.config.capacity_bytes - p.bytes;
      needed = size > free ? size - free : 0;
      if (needed == 0)
        decision = Decision::AdmitFreeSpace;
      else {
        p.policy->plan_admission(slot, needed, now, p.plan);
        if (!p.plan.admit) {
          CLACHE_REQUIRE(p.plan.victims.empty() && is_reject(p.plan.decision),
                         "invalid rejection plan");
          decision = p.plan.decision;
        } else {
          std::unordered_set<SlotId> distinct;
          std::uint64_t total = 0;
          for (SlotId v : p.plan.victims) {
            CLACHE_REQUIRE(v != slot && p.residents.contains(v) && distinct.insert(v).second,
                           "invalid victim");
            total += p.meta[v].charged_bytes;
          }
          CLACHE_REQUIRE(total >= needed && total == p.plan.victim_bytes,
                         "insufficient victim bytes");
          for (SlotId v : p.plan.victims) {
            p.removed.push_back(p.meta[v].key);
            p.remove(v, RemovalReason::Evicted, now);
          }
          decision = Decision::AdmitWithEviction;
        }
      }
      if (is_admit(decision)) {
        p.meta.mark_resident(slot);
        p.bytes += size;
        p.residents.insert(slot);
        if (ttl) {
          const auto max_delta =
              std::numeric_limits<std::int64_t>::max() - std::max<std::int64_t>(now.ns(), 0);
          m.expiry = ttl->count() >= max_delta ? kNever : now + *ttl;
          if (m.expiry != kNever) p.expiry.emplace(std::make_pair(m.expiry, slot), key);
        }
        p.policy->on_insert(slot, now);
        ++p.stats.admissions;
      }
    }
  }
  if (is_reject(decision)) ++p.stats.admission_rejections;
  p.record(slot, decision, now, needed);
  p.meta.unpin(slot);
  AdmissionResult result{decision,  is_admit(decision),     p.expired,
                         p.removed, p.plan.candidate_value, p.plan.threshold_value};
  p.finish(now);
  return result;
}
void CacheEngine::abort_miss(KeyId key, TimePoint now) {
  auto& p = *impl_;
  p.begin(now);
  auto slot = p.meta.find(key);
  CLACHE_REQUIRE(slot != kNoSlot && p.meta[slot].pins, "abort_miss requires a missing lookup");
  p.meta.unpin(slot);
  p.finish(now);
}
std::span<const KeyId> CacheEngine::invalidate(KeyId key, TimePoint now) {
  auto& p = *impl_;
  p.begin(now);
  auto slot = p.meta.find(key);
  if (slot != kNoSlot) {
    auto& m = p.meta[slot];
    if (m.resident) {
      p.removed.push_back(key);
      p.remove(slot, RemovalReason::Invalidated, now);
    }
    ++m.generation;
    m.cost = {};
  }
  p.finish(now);
  return p.removed;
}
void CacheEngine::observe_cost_hint(KeyId key, double cost_ms, TimePoint now) {
  auto& p = *impl_;
  p.begin(now);
  auto slot = p.meta.lookup_or_observe(key, now);
  if (std::isfinite(cost_ms) && cost_ms >= 0) {
    p.cost.observe(p.meta[slot].cost, cost_ms);
    if (p.meta[slot].resident) {
      p.policy->on_cost_update(slot, now);
      ++p.stats.score_updates;
    }
  }
  p.finish(now);
}
const EngineStats& CacheEngine::stats() const noexcept { return impl_->stats; }
std::uint64_t CacheEngine::capacity_bytes() const noexcept { return impl_->config.capacity_bytes; }
std::uint64_t CacheEngine::bytes_resident() const noexcept { return impl_->bytes; }
std::size_t CacheEngine::resident_count() const noexcept { return impl_->meta.resident_count(); }
std::uint64_t CacheEngine::metadata_bytes() const noexcept {
  return impl_->meta.metadata_bytes() + impl_->policy->metadata_bytes() +
         impl_->expiry.size() * (sizeof(TimePoint) + sizeof(SlotId) + 4 * sizeof(void*));
}
std::string_view CacheEngine::policy_name() const noexcept { return impl_->policy->name(); }
std::optional<KeyExplanation> CacheEngine::explain(KeyId key, TimePoint now) const {
  const auto& p = *impl_;
  auto slot = p.meta.find(key);
  if (slot == kNoSlot) return std::nullopt;
  const auto& m = p.meta[slot];
  return KeyExplanation{key,
                        m.resident,
                        m.charged_bytes,
                        p.freq.rate(m.freq, now),
                        p.cost.estimate_ms(m.cost),
                        p.policy->value(slot, now),
                        m.access_count,
                        m.last_access,
                        m.expiry};
}
bool CacheEngine::check_invariants(TimePoint now) const { return impl_->invariants(now); }
}  // namespace clache
