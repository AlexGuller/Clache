// Frequency (F) and miss-cost (C) estimators (Spec §6).
//
// Estimators are stateless apart from config and the time origin T0; all
// per-key state lives in EntryMetadata. Implemented on feat/estimators.
#pragma once

#include <array>
#include <cstdint>
#include <limits>

#include "clache/config.h"
#include "clache/types.h"

namespace clache {

inline constexpr std::uint32_t kMaxBuckets = 8;

struct FrequencyState {
  // Forward-decay mode: ln sum_k exp(u(t_k)), u(t) = (t - T0) / tau.
  double log_mass = -std::numeric_limits<double>::infinity();
  // Rolling-bucket mode: epoch of the most recent record; ring index = epoch % B.
  std::uint32_t last_epoch = 0;
  std::array<std::uint16_t, kMaxBuckets> buckets{};  // saturating at 65535
  bool recorded = false;                             // false until the first record() (both modes)
};
static_assert(sizeof(FrequencyState) <= 40);

class FrequencyEstimator {
 public:
  using Mode = FrequencyConfig::Mode;

  explicit FrequencyEstimator(const FrequencyConfig& config);

  // The engine calls this once, with the time of its first operation, before
  // any record/rate call. Epochs and u(t) are measured from here.
  void set_origin(TimePoint t0);
  TimePoint origin() const noexcept;

  // Counts one request at `now` (Spec §6.1.1 / §6.1.3). Returns true if a
  // bucket counter saturated.
  bool record(FrequencyState& state, TimePoint now) const;

  // F in requests/second. Pure: never mutates `state`.
  double rate(const FrequencyState& state, TimePoint now) const;

  // Ranking key for ln F:
  //   bucket mode: ln rate(state, now)        (valid until the next epoch)
  //   decay mode:  log_mass - ln tau          (time-invariant)
  // -inf when the rate is zero.
  double log_rate_key(const FrequencyState& state, TimePoint now) const;

  // Decay mode: u(now) = (now - T0) / tau. Converts a stored key back to ln F:
  // ln F(now) = log_rate_key - u(now). Zero in bucket mode.
  double time_offset(TimePoint now) const;

  // Bucket mode: floor((now - T0) / bucket_seconds).
  std::uint32_t epoch(TimePoint now) const;

  Mode mode() const noexcept { return config_.mode; }
  const FrequencyConfig& config() const noexcept { return config_; }

 private:
  FrequencyConfig config_;
  TimePoint t0_{};
};

struct CostState {
  double ewma_ms = 0.0;
  std::uint32_t samples = 0;
};

class MissCostEstimator {
 public:
  explicit MissCostEstimator(const MissCostConfig& config);

  // Clamps to [min_ms, max_ms]; first sample initialises the EWMA (Spec §6.2).
  void observe(CostState& state, double observed_ms) const;

  // default_ms with no samples; prior-limited below min_samples; else EWMA.
  double estimate_ms(const CostState& state) const;

  const MissCostConfig& config() const noexcept { return config_; }

 private:
  MissCostConfig config_;
};

}  // namespace clache
