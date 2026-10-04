#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "clache/engine.h"
#include "clache/rng.h"

namespace clache {
namespace {
constexpr std::size_t kKeys = 50;
constexpr std::uint64_t kOperations = 10'000;

std::uint64_t SeedCount() {
  if (const char* text = std::getenv("CLACHE_PROPERTY_SEEDS")) {
    const auto count = std::stoull(text);
    if (count == 0) throw std::invalid_argument("CLACHE_PROPERTY_SEEDS must be positive");
    return count;
  }
#ifdef NDEBUG
  return 1000;
#else
  return 10;
#endif
}

struct ReferenceState {
  struct History {
    std::vector<std::int64_t> timestamps;
    double cost = 0;
    std::uint32_t samples = 0;
  };
  std::array<History, kKeys> keys;
  EngineConfig config;
  Decision actual_decision = Decision::RejectPolicy;
  std::vector<KeyId> actual_victims;
  std::uint64_t skipped = 0;

  void observe(KeyId key, double cost) {
    auto& h = keys[key];
    const auto& c = config.miss_cost;
    const double sample = std::clamp(cost, c.min_ms, c.max_ms);
    h.cost = h.samples == 0 ? sample : c.ewma_alpha * sample + (1 - c.ewma_alpha) * h.cost;
    ++h.samples;
  }
  double score(const EntryMetadata& entry, TimePoint now) const {
    const auto& h = keys[entry.key];
    const auto& f = config.frequency;
    double frequency = 0;
    if (f.mode == FrequencyConfig::Mode::RollingBuckets) {
      const auto epoch = now.ns() / (static_cast<std::int64_t>(f.bucket_seconds) * 1'000'000'000);
      const auto first_epoch =
          std::max<std::int64_t>(0, epoch - f.window_seconds / f.bucket_seconds + 1);
      const auto first_ns = first_epoch * f.bucket_seconds * 1'000'000'000;
      const auto first = std::lower_bound(h.timestamps.begin(), h.timestamps.end(), first_ns);
      frequency = static_cast<double>(h.timestamps.end() - first) / f.window_seconds;
    } else {
      // Retain the complete history. Exponentials below the double underflow limit
      // contribute exactly zero, so their prefix can be skipped without approximation.
      const double tau_ns = f.decay_tau_seconds * 1e9;
      const auto cutoff = static_cast<std::int64_t>(static_cast<double>(now.ns()) - 746 * tau_ns);
      auto first = std::lower_bound(h.timestamps.begin(), h.timestamps.end(), cutoff);
      long double mass = 0;
      for (; first != h.timestamps.end(); ++first) {
        mass += std::exp(-static_cast<double>(now.ns() - *first) / tau_ns);
      }
      frequency = static_cast<double>(mass) / f.decay_tau_seconds;
    }
    const auto& c = config.miss_cost;
    double cost = h.samples == 0 ? c.default_ms : h.cost;
    if (h.samples != 0 && h.samples < c.min_samples) cost = std::min(cost, c.default_ms);
    // Admission compares the prescribed log key (§10.4), including its floating-point
    // rounding. Mathematically equal products F*C/S need not have bit-identical keys.
    // Histories and costs are reconstructed independently of the production estimators.
    return std::log(frequency) + std::log(cost) -
           config.value.size_exponent * std::log(static_cast<double>(entry.charged_bytes));
  }
};

class ReferenceClachePolicy final : public Policy {
 public:
  ReferenceClachePolicy(const PolicyContext& context, std::shared_ptr<ReferenceState> state)
      : context_(context), state_(std::move(state)) {}
  std::string_view name() const override { return "test_reference_clache"; }
  void on_access(SlotId slot, TimePoint now) override {
    if (slot != kNoSlot) state_->keys[(*context_.meta)[slot].key].timestamps.push_back(now.ns());
  }
  void on_hit(SlotId, TimePoint) override {}
  void on_insert(SlotId slot, TimePoint) override { residents_.push_back(slot); }
  void on_remove(SlotId slot, RemovalReason) override {
    residents_.erase(std::find(residents_.begin(), residents_.end(), slot));
  }
  void plan_admission(SlotId candidate, std::uint64_t needed, TimePoint now,
                      AdmissionPlan& plan) override {
    struct Ranked {
      SlotId slot;
      double value;
      std::uint64_t seq;
      KeyId key;
    };
    std::vector<Ranked> ranked;
    for (const auto slot : residents_) {
      const auto& entry = (*context_.meta)[slot];
      ranked.push_back({slot, state_->score(entry, now), entry.last_access_seq, entry.key});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
      if (a.value != b.value) return a.value < b.value;
      if (a.seq != b.seq) return a.seq < b.seq;
      return a.key < b.key;
    });
    plan.reset();
    const auto candidate_score = state_->score((*context_.meta)[candidate], now);
    plan.candidate_value = std::exp(candidate_score);
    plan.decision = Decision::RejectCandidateNotHigher;
    std::uint64_t bytes = 0;
    bool near_tie = false;
    auto close = [](double a, double b) {
      return std::abs(a - b) < 1e-9 * std::max(std::abs(a), std::abs(b));
    };
    if (ranked.size() > 1) near_tie = close(std::exp(ranked[0].value), std::exp(ranked[1].value));
    for (const auto& victim : ranked) {
      near_tie = near_tie || close(std::exp(victim.value), plan.candidate_value);
      plan.threshold_value = std::exp(victim.value);
      if (victim.value >= candidate_score) break;
      if (state_->config.admission.max_victims != 0 &&
          plan.victims.size() == state_->config.admission.max_victims) {
        plan.decision = Decision::RejectTooManyVictims;
        break;
      }
      plan.victims.push_back(victim.slot);
      bytes += (*context_.meta)[victim.slot].charged_bytes;
      if (bytes >= needed) break;
    }
    if (bytes >= needed) {
      plan.admit = true;
      plan.decision = Decision::AdmitWithEviction;
      plan.victim_bytes = bytes;
    } else {
      plan.victims.clear();
    }
    if (state_->config.frequency.mode == FrequencyConfig::Mode::ForwardDecay && near_tie) {
      // Synchronize only the explicitly allowed ambiguous decision, so subsequent
      // comparisons still start with identical resident sets and telemetry.
      ++state_->skipped;
      plan.reset();
      plan.decision = state_->actual_decision;
      plan.admit = is_admit(plan.decision);
      for (const auto key : state_->actual_victims) {
        const auto slot = context_.meta->find(key);
        plan.victims.push_back(slot);
        plan.victim_bytes += (*context_.meta)[slot].charged_bytes;
      }
    }
  }
  double value(SlotId slot, TimePoint now) const override {
    return std::exp(state_->score((*context_.meta)[slot], now));
  }
  std::uint64_t metadata_bytes() const override { return 0; }
  bool check_invariants() const override {
    return residents_.size() == context_.meta->resident_count();
  }

 private:
  PolicyContext context_;
  std::shared_ptr<ReferenceState> state_;
  std::vector<SlotId> residents_;
};

// A thread-local handoff avoids persistent test state or cross-thread interference
// when the normal policy registry constructs the reference engine.
thread_local std::shared_ptr<ReferenceState> constructing_reference;
CLACHE_REGISTER_POLICY("test_reference_clache", [](const PolicyConfig&,
                                                   const PolicyContext& context) {
  return std::make_unique<ReferenceClachePolicy>(context, constructing_reference);
});

EngineConfig PropertyConfig(std::uint64_t seed, FrequencyConfig::Mode mode) {
  EngineConfig config;
  config.capacity_bytes = 200;
  config.frequency.mode = mode;
  config.observation = {kKeys, std::numeric_limits<std::uint32_t>::max()};
  config.admission.min_observations = static_cast<std::uint32_t>(seed % 3 + 1);
  config.admission.max_victims = static_cast<std::uint32_t>(seed % 5);
  config.miss_cost.min_samples = static_cast<std::uint32_t>(seed % 3 + 1);
  config.seed = seed;
  return config;
}
std::vector<KeyId> Copy(std::span<const KeyId> keys) { return {keys.begin(), keys.end()}; }

TEST(ReferenceComparison, RationallyEqualValuesUsePrescribedLogKeyComparison) {
  EngineConfig config;
  config.capacity_bytes = 26;
  CacheEngine engine(config, "clache", {});
  const auto now = TimePoint::from_ns(0);
  const auto resident = engine.lookup(1, now, 26);
  ASSERT_TRUE(engine.complete_miss(1, 26, 0.01, std::nullopt, resident.generation, now).admitted);
  ASSERT_TRUE(engine.lookup(1, now, 26).hit);
  const auto candidate = engine.lookup(2, now, 13);
  const auto result = engine.complete_miss(2, 13, 0.01, std::nullopt, candidate.generation, now);

  // F=1/W, S=13 and F=2/W, S=26 give equal rational products F*C/S.
  // They need not yield bit-identical log keys on every platform's libm. Only
  // bit-identical K ties reject; the independent oracle must preserve that rule.
  const double candidate_key = std::log(1.0 / config.frequency.window_seconds) +
                               std::log(config.miss_cost.min_ms) - std::log(13.0);
  const double resident_key = std::log(2.0 / config.frequency.window_seconds) +
                              std::log(config.miss_cost.min_ms) - std::log(26.0);
  const bool admit = candidate_key > resident_key;
  EXPECT_EQ(result.decision,
            admit ? Decision::AdmitWithEviction : Decision::RejectCandidateNotHigher);
  EXPECT_EQ(Copy(result.evicted), admit ? std::vector<KeyId>{1} : std::vector<KeyId>{});
}

TEST(ReferenceComparison, ClacheBucketsAndDecayAcross1000Seeds) {
  const auto seeds = SeedCount();
  std::uint64_t decisions = 0, decay_decisions = 0, skipped = 0;
  for (const auto mode :
       {FrequencyConfig::Mode::RollingBuckets, FrequencyConfig::Mode::ForwardDecay}) {
    for (std::uint64_t seed = 0; seed < seeds; ++seed) {
      SCOPED_TRACE(::testing::Message() << "seed=" << seed << " mode=" << static_cast<int>(mode));
      auto config = PropertyConfig(seed, mode);
      auto state = std::make_shared<ReferenceState>();
      state->config = config;
      constructing_reference = state;
      CacheEngine actual(config, "clache", {});
      CacheEngine reference(config, "test_reference_clache", {});
      constructing_reference.reset();
      Xoshiro256ss rng(seed);
      std::int64_t nanos = 0;
      for (std::uint64_t op = 0; op < kOperations; ++op) {
        const auto key = rng.uniform_below(kKeys);
        // Deletes share the last purged request time; lookup advances time and purges TTLs.
        if (op != 0 && rng.uniform_below(10) == 0) {
          ASSERT_EQ(Copy(actual.invalidate(key, TimePoint::from_ns(nanos))),
                    Copy(reference.invalidate(key, TimePoint::from_ns(nanos))))
              << "op=" << op;
          state->keys[key].samples = 0;
          state->keys[key].cost = 0;
          continue;
        }
        if (op != 0) {
          const auto jump = rng.uniform_below(20);
          nanos += jump == 0 ? 601'000'000'000 : jump < 4 ? 60'000'000'000 : 1'000'000;
        }
        const auto now = TimePoint::from_ns(nanos);
        const auto size = 1 + rng.uniform_below(40);
        const auto lookup = actual.lookup(key, now, size);
        const auto expired = Copy(lookup.expired), replaced = Copy(lookup.replaced);
        const auto reference_lookup = reference.lookup(key, now, size);
        ASSERT_EQ(lookup.hit, reference_lookup.hit) << "op=" << op;
        ASSERT_EQ(expired, Copy(reference_lookup.expired)) << "op=" << op;
        ASSERT_EQ(replaced, Copy(reference_lookup.replaced)) << "op=" << op;
        if (!lookup.hit) {
          const double cost = 0.1 + rng.uniform01() * 1000;
          std::optional<Nanos> ttl;
          if (rng.uniform_below(4) == 0)
            ttl = Nanos(static_cast<std::int64_t>(1 + rng.uniform_below(1200)) * 1'000'000'000);
          const auto result = actual.complete_miss(key, size, cost, ttl, lookup.generation, now);
          state->actual_decision = result.decision;
          state->actual_victims = Copy(result.evicted);
          state->observe(key, cost);
          const auto expected =
              reference.complete_miss(key, size, cost, ttl, reference_lookup.generation, now);
          ASSERT_EQ(result.decision, expected.decision) << "op=" << op;
          ASSERT_EQ(state->actual_victims, Copy(expected.evicted)) << "op=" << op;
          ++decisions;
          if (mode == FrequencyConfig::Mode::ForwardDecay) ++decay_decisions;
        }
        ASSERT_EQ(actual.bytes_resident(), reference.bytes_resident()) << "op=" << op;
        ASSERT_EQ(actual.resident_count(), reference.resident_count()) << "op=" << op;
        ASSERT_LE(actual.bytes_resident(), config.capacity_bytes);
        ASSERT_TRUE(actual.check_invariants(now)) << "op=" << op;
        ASSERT_TRUE(reference.check_invariants(now)) << "op=" << op;
      }
      skipped += state->skipped;
    }
  }
  RecordProperty("seeds_per_mode", seeds);
  RecordProperty("operations_per_seed", kOperations);
  RecordProperty("decisions", decisions);
  RecordProperty("decay_near_ties_skipped", skipped);
  RecordProperty("decay_decisions", decay_decisions);
  EXPECT_LT(static_cast<double>(skipped), static_cast<double>(decay_decisions) * 0.001);
}

TEST(ReferenceComparison, LruAgainstListAndAvailableBaselineInvariants) {
  for (const auto& name : registered_policies()) {
    if (name.starts_with("test_")) continue;
    for (std::uint64_t seed = 0; seed < 10; ++seed) {
      SCOPED_TRACE(::testing::Message() << name << " seed=" << seed);
      auto config = PropertyConfig(seed, FrequencyConfig::Mode::RollingBuckets);
      config.admission.min_observations = 1;
      CacheEngine engine(config, name, {});
      Xoshiro256ss rng(seed);
      std::list<KeyId> lru;
      std::array<std::uint64_t, kKeys> sizes{};
      std::uint64_t bytes = 0;
      for (std::uint64_t op = 0; op < kOperations; ++op) {
        const auto key = rng.uniform_below(kKeys);
        const auto now = TimePoint::from_ns(static_cast<std::int64_t>(op));
        if (rng.uniform_below(10) == 0) {
          const auto removed = Copy(engine.invalidate(key, now));
          if (name == "lru") {
            const auto found = std::find(lru.begin(), lru.end(), key);
            ASSERT_EQ(!removed.empty(), found != lru.end());
            if (found != lru.end()) {
              bytes -= sizes[key];
              lru.erase(found);
            }
          }
        } else {
          const auto size = 1 + rng.uniform_below(40);
          const auto result = engine.lookup(key, now, size);
          if (name == "lru") {
            auto found = std::find(lru.begin(), lru.end(), key);
            const bool hit = found != lru.end() && sizes[key] == size;
            ASSERT_EQ(result.hit, hit);
            if (found != lru.end()) {
              bytes -= sizes[key];
              lru.erase(found);
            }
            if (hit) {
              lru.push_front(key);
              bytes += size;
            }
          }
          if (!result.hit) {
            const auto admitted =
                engine.complete_miss(key, size, 1, std::nullopt, result.generation, now);
            if (name == "lru") {
              std::vector<KeyId> victims;
              while (bytes + size > config.capacity_bytes) {
                const auto victim = lru.back();
                lru.pop_back();
                bytes -= sizes[victim];
                victims.push_back(victim);
              }
              ASSERT_EQ(Copy(admitted.evicted), victims);
              ASSERT_TRUE(admitted.admitted);
              lru.push_front(key);
              sizes[key] = size;
              bytes += size;
            }
          }
        }
        ASSERT_LE(engine.bytes_resident(), config.capacity_bytes);
        ASSERT_TRUE(engine.check_invariants(now));
        if (name == "lru") {
          ASSERT_EQ(bytes, engine.bytes_resident());
          ASSERT_EQ(lru.size(), engine.resident_count());
        }
      }
    }
  }
}
}  // namespace
}  // namespace clache
