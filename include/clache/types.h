// Core vocabulary types shared by every Clache component (Spec §5).
#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

// Determinism and -inf heap keys both depend on IEEE semantics (Spec §2 #12).
#if defined(__FAST_MATH__)
#error "Clache must not be compiled with -ffast-math / -Ofast"
#endif

namespace clache {

// Trace key_id, or XXH3_64(key) inside Cache.
using KeyId = std::uint64_t;

// Dense index into MetadataStore. Recycled after a key leaves the observation
// table, so anything that outlives a request must be keyed by KeyId instead.
using SlotId = std::uint32_t;
inline constexpr SlotId kNoSlot = std::numeric_limits<SlotId>::max();

using Nanos = std::chrono::nanoseconds;

// Engine time: nanoseconds since an arbitrary epoch. The simulator uses trace
// timestamps; Cache uses steady_clock. Never wall-clock.
struct TimePoint {
  Nanos since_epoch{0};

  static constexpr TimePoint from_ns(std::int64_t ns) { return TimePoint{Nanos{ns}}; }
  constexpr std::int64_t ns() const { return since_epoch.count(); }

  friend constexpr auto operator<=>(const TimePoint&, const TimePoint&) = default;
  friend constexpr TimePoint operator+(TimePoint t, Nanos d) {
    return TimePoint{t.since_epoch + d};
  }
  friend constexpr Nanos operator-(TimePoint a, TimePoint b) {
    return a.since_epoch - b.since_epoch;
  }
};
inline constexpr TimePoint kNever{Nanos::max()};

// Seconds as double, for rate / window arithmetic.
constexpr double to_seconds(Nanos d) { return static_cast<double>(d.count()) * 1e-9; }

// Trace costs are integer microseconds; the engine works in milliseconds.
constexpr double us_to_ms(std::uint64_t us) { return static_cast<double>(us) / 1000.0; }

enum class RemovalReason : std::uint8_t {
  Evicted,
  Expired,
  Invalidated,
  Replaced,  // resident key requested with a different size (Spec §2 #9)
};
inline constexpr std::size_t kRemovalReasonCount = 4;

enum class Decision : std::uint8_t {
  Hit,
  AdmitFreeSpace,  // fit without eviction
  AdmitWithEviction,
  RejectOversized,              // size > max_object_bytes or > capacity
  RejectInsufficientHistory,    // access_count < admission.min_observations
  RejectCandidateNotHigher,     // a required victim has value >= candidate
  RejectTooManyVictims,         // admission.max_victims exceeded
  RejectInvalidatedDuringLoad,  // generation changed between lookup and complete_miss
  RejectHashCollision,          // Cache only: another key owns this KeyId
  RejectPolicy,                 // generic, for policies with their own rule
};
inline constexpr std::size_t kDecisionCount = 10;

constexpr bool is_admit(Decision d) {
  return d == Decision::AdmitFreeSpace || d == Decision::AdmitWithEviction;
}
constexpr bool is_reject(Decision d) { return d != Decision::Hit && !is_admit(d); }

// Reason string used in decision logs, e.g.
// "CANDIDATE_VALUE_NOT_HIGHER_THAN_REQUIRED_VICTIMS".
std::string_view to_string(Decision d) noexcept;
// "HIT", "ADMIT" or "REJECT".
std::string_view decision_class(Decision d) noexcept;
// "EVICTED", "EXPIRED", "INVALIDATED", "REPLACED".
std::string_view to_string(RemovalReason r) noexcept;

}  // namespace clache
