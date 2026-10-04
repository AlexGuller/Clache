// CacheEngine: the deterministic, single-threaded, value-free decision core
// shared by clache_sim and Cache (Spec §3, §12). Implemented on feat/engine.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "clache/config.h"
#include "clache/estimators.h"
#include "clache/metadata.h"
#include "clache/policy.h"
#include "clache/telemetry.h"
#include "clache/types.h"

namespace clache {

struct LookupResult {
  bool hit = false;
  SlotId slot = kNoSlot;
  std::uint32_t generation = 0;     // pass back to complete_miss()
  std::span<const KeyId> expired;   // purged during this call: erase from storage
  std::span<const KeyId> replaced;  // resident removed due to size change
};

struct AdmissionResult {
  Decision decision = Decision::RejectPolicy;
  bool admitted = false;
  std::span<const KeyId> expired;  // purged at the start of this call
  std::span<const KeyId> evicted;  // erase these from storage
  double candidate_value = 0.0;
  double threshold_value = 0.0;
};

struct EngineStats {
  std::uint64_t requests = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t admissions = 0;
  std::uint64_t admission_rejections = 0;
  std::array<std::uint64_t, kDecisionCount> by_decision{};  // indexed by Decision
  std::uint64_t evictions = 0;
  std::uint64_t expirations = 0;
  std::uint64_t invalidations = 0;
  std::uint64_t size_changes = 0;
  std::uint64_t bytes_evicted = 0;
  std::uint64_t score_updates = 0;
  std::uint64_t heap_rebuilds = 0;
  std::uint64_t freq_saturations = 0;
  double origin_cost_ms_total = 0.0;           // sum of observed miss costs
  double origin_cost_ms_saved_estimate = 0.0;  // sum of estimate_ms at hit time
};

struct KeyExplanation {
  KeyId key = 0;
  bool resident = false;
  std::uint64_t charged_bytes = 0;
  double frequency_rps = 0.0;
  double miss_cost_ms = 0.0;
  double value = 0.0;  // Policy::value(); NaN if the policy has no score
  std::uint32_t access_count = 0;
  TimePoint last_access{};
  TimePoint expiry = kNever;
};

class CacheEngine {
 public:
  // Throws std::invalid_argument with all engine/policy validation errors, or if the
  // policy name is unknown. `decisions` may be null; it must outlive the engine.
  // expected_entries is a sizing hint for policies (e.g. TinyLFU); zero selects
  // their fallback estimate. Existing three/four-argument callers remain valid.
  CacheEngine(EngineConfig config, std::string_view policy_name, const PolicyConfig& policy_config,
              DecisionSink* decisions = nullptr, std::uint64_t expected_entries = 0);
  ~CacheEngine();
  CacheEngine(const CacheEngine&) = delete;
  CacheEngine& operator=(const CacheEngine&) = delete;

  // `now` must be non-decreasing across all non-const calls; otherwise
  // std::invalid_argument. Returned spans point into engine-owned scratch and
  // stay valid until the next non-const call. Every mutating operation purges
  // expired residents before applying its requested action.

  // Step 1 of a request: purge expired, record the access, report hit/miss.
  // `expected_bytes` (charged) lets the simulator detect size changes. On a
  // miss the key's slot is pinned until complete_miss() or abort_miss().
  LookupResult lookup(KeyId key, TimePoint now,
                      std::optional<std::uint64_t> expected_bytes = std::nullopt);

  // Step 2, miss only: the loaded object is presented for admission. Must
  // follow lookup() of the same key; calls for other keys may interleave.
  // `cost_ms` is the measured loader latency or the caller's cost hint.
  AdmissionResult complete_miss(KeyId key, std::uint64_t charged_bytes, double cost_ms,
                                std::optional<Nanos> ttl, std::uint32_t generation, TimePoint now);

  // Releases the pin taken by a missing lookup() whose load failed. No
  // access/cost telemetry is recorded; expiry counters can still advance.
  void abort_miss(KeyId key, TimePoint now);

  // Removes the resident (if any), bumps the generation, resets cost state.
  // Returns the invalidated key (0 or 1 entries) for storage erasure. Keys
  // expired first, including the target if expired, appear in expired_keys().
  std::span<const KeyId> invalidate(KeyId key, TimePoint now);

  // Applies a cost observation without recording an access.
  void observe_cost_hint(KeyId key, double cost_ms, TimePoint now);

  // Keys purged at the start of the latest mutating call. In particular, callers
  // must erase these from storage after invalidate(), observe_cost_hint(), and
  // abort_miss(), whose return values do not report expiry. Same list as the
  // expired span returned by lookup()/complete_miss(). The span remains valid
  // until the next non-const call or engine destruction; initially empty.
  std::span<const KeyId> expired_keys() const noexcept;

  const EngineStats& stats() const noexcept;
  std::uint64_t capacity_bytes() const noexcept;
  std::uint64_t bytes_resident() const noexcept;
  std::size_t resident_count() const noexcept;
  // MetadataStore + policy + expiry index.
  std::uint64_t metadata_bytes() const noexcept;
  std::string_view policy_name() const noexcept;

  std::optional<KeyExplanation> explain(KeyId key, TimePoint now) const;

  // Spec §19.4. Returns false (and never aborts) so tests can assert on it.
  bool check_invariants(TimePoint now) const;

 private:
  // Owned by feat/engine; private layout may change freely.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace clache
