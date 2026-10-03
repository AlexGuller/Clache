// Policy interface, admission plans and the policy registry (Spec §9).
#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "clache/config.h"
#include "clache/estimators.h"
#include "clache/metadata.h"
#include "clache/types.h"

namespace clache {

// Read-only view of engine state handed to a policy at construction. All
// pointers outlive the policy.
struct PolicyContext {
  const MetadataStore* meta = nullptr;
  const FrequencyEstimator* freq = nullptr;
  const MissCostEstimator* cost = nullptr;
  const EngineConfig* engine = nullptr;
  std::uint64_t capacity_bytes = 0;
  std::uint64_t seed = 0;              // for policies that sample (redis_*)
  std::uint64_t expected_entries = 0;  // sizing hint (TinyLFU sketch); 0 = unknown
};

struct AdmissionPlan {
  bool admit = false;
  Decision decision = Decision::RejectPolicy;
  std::vector<SlotId> victims;  // eviction order; engine clears before each call
  std::uint64_t victim_bytes = 0;
  double candidate_value = std::numeric_limits<double>::quiet_NaN();
  double threshold_value = std::numeric_limits<double>::quiet_NaN();

  // Keeps the victims buffer's capacity.
  void reset() noexcept {
    admit = false;
    decision = Decision::RejectPolicy;
    victims.clear();
    victim_bytes = 0;
    candidate_value = std::numeric_limits<double>::quiet_NaN();
    threshold_value = std::numeric_limits<double>::quiet_NaN();
  }
};

// Contract (Spec §9.1):
//  1. Per-entry state only for resident slots, in SlotId-indexed vectors grown
//     on demand and cleared in on_remove. Key-level history (sketches, ghost
//     queues) is keyed by KeyId, because slots are recycled.
//  2. plan_admission is only called with bytes_needed > 0, size <= capacity,
//     prefilters passed, and bytes_needed <= resident bytes.
//  3. admit == false  =>  victims empty; a rejected plan removes nothing.
//  4. admit == true   =>  victims distinct, resident, not the candidate, and
//     sum >= bytes_needed. The engine checks this (CLACHE_REQUIRE).
//  5. Always-admit policies may commit internal reordering inside
//     plan_admission; their on_remove(victim, Evicted) must then tolerate a
//     victim that is already detached from internal lists.
//  6. Policies that may reject must not mutate state in plan_admission
//     (scratch buffers excepted).
//  7. Deterministic given the same call sequence and seed.
class Policy {
 public:
  virtual ~Policy() = default;

  virtual std::string_view name() const = 0;

  // Every request (hit or miss), after telemetry is recorded, before anything else.
  virtual void on_access(SlotId /*slot*/, TimePoint /*now*/) {}

  // A resident key was hit.
  virtual void on_hit(SlotId slot, TimePoint now) = 0;

  // A non-resident candidate needs bytes_needed > 0 bytes freed. Fill `plan`.
  virtual void plan_admission(SlotId candidate, std::uint64_t bytes_needed, TimePoint now,
                              AdmissionPlan& plan) = 0;

  // The candidate was admitted (after all victims were removed).
  virtual void on_insert(SlotId slot, TimePoint now) = 0;

  // A resident was removed for any reason. Its metadata is still valid during the call.
  virtual void on_remove(SlotId slot, RemovalReason reason) = 0;

  // A resident's cost estimate changed via a hint (no access recorded).
  virtual void on_cost_update(SlotId /*slot*/, TimePoint /*now*/) {}

  // Current policy score for explanations; NaN if not meaningful.
  virtual double value(SlotId /*slot*/, TimePoint /*now*/) const {
    return std::numeric_limits<double>::quiet_NaN();
  }

  virtual std::uint64_t metadata_bytes() const = 0;

  virtual bool check_invariants() const { return true; }
};

// ---- Registry ----------------------------------------------------------------

using PolicyFactory =
    std::function<std::unique_ptr<Policy>(const PolicyConfig&, const PolicyContext&)>;

// Registers a factory under `name`. Throws std::logic_error on duplicates.
// Returns true so it can initialise a static (see CLACHE_REGISTER_POLICY).
bool register_policy(std::string_view name, PolicyFactory factory);

// Throws std::invalid_argument (listing registered names) if unknown.
std::unique_ptr<Policy> make_policy(std::string_view name, const PolicyConfig& config,
                                    const PolicyContext& context);

bool is_registered_policy(std::string_view name);

// Sorted by name.
std::vector<std::string> registered_policies();

}  // namespace clache

#define CLACHE_DETAIL_CONCAT_INNER(a, b) a##b
#define CLACHE_DETAIL_CONCAT(a, b) CLACHE_DETAIL_CONCAT_INNER(a, b)

// Registers a policy at static-initialisation time. Use at namespace scope in
// the policy's .cpp:
//
//   CLACHE_REGISTER_POLICY("lru", [](const PolicyConfig& c, const PolicyContext& ctx) {
//     return std::make_unique<LruPolicy>(c, ctx);
//   });
//
// Works from inside the clache_core static library because consumers link it
// whole (clache::core, see src/CMakeLists.txt).
#define CLACHE_REGISTER_POLICY(name, ...)                                                 \
  namespace {                                                                             \
  [[maybe_unused]] const bool CLACHE_DETAIL_CONCAT(clache_policy_registered_, __LINE__) = \
      ::clache::register_policy(name, __VA_ARGS__);                                       \
  }
