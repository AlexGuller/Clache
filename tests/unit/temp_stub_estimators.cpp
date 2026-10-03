// TEMPORARY TEST-ONLY STUB. DELETE THIS FILE when feat/estimators (Person A)
// is merged: the build will then fail with duplicate symbols for
// FrequencyEstimator / MissCostEstimator, which is the signal.
//
// Exists only so policy branches can unit-test before the real estimators
// land. Follows Spec §6.1/§6.2 closely enough for tests; not production code.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "clache/estimators.h"

namespace clache {
namespace {

constexpr double kNegInf = -std::numeric_limits<double>::infinity();

double logaddexp(double a, double b) {
  if (a == kNegInf) return b;
  if (b == kNegInf) return a;
  return std::max(a, b) + std::log1p(std::exp(-std::fabs(a - b)));
}

}  // namespace

FrequencyEstimator::FrequencyEstimator(const FrequencyConfig& config) : config_(config) {}

void FrequencyEstimator::set_origin(TimePoint t0) { t0_ = t0; }

TimePoint FrequencyEstimator::origin() const noexcept { return t0_; }

std::uint32_t FrequencyEstimator::epoch(TimePoint now) const {
  if (now <= t0_) return 0;
  const auto bucket_ns = static_cast<std::int64_t>(config_.bucket_seconds) * 1'000'000'000;
  return static_cast<std::uint32_t>((now - t0_).count() / bucket_ns);
}

double FrequencyEstimator::time_offset(TimePoint now) const {
  if (config_.mode != Mode::ForwardDecay) return 0.0;
  return to_seconds(now - t0_) / config_.decay_tau_seconds;
}

bool FrequencyEstimator::record(FrequencyState& state, TimePoint now) const {
  if (config_.mode == Mode::ForwardDecay) {
    state.log_mass = logaddexp(state.log_mass, time_offset(now));
    state.recorded = true;
    return false;
  }
  const std::uint32_t buckets = config_.window_seconds / config_.bucket_seconds;
  const std::uint32_t e = epoch(now);
  if (!state.recorded || e - state.last_epoch >= buckets) {
    state.buckets.fill(0);
  } else {
    for (std::uint32_t ep = state.last_epoch + 1; ep <= e; ++ep) state.buckets[ep % buckets] = 0;
  }
  auto& slot = state.buckets[e % buckets];
  const bool saturated = slot == std::numeric_limits<std::uint16_t>::max();
  if (!saturated) ++slot;
  state.last_epoch = e;
  state.recorded = true;
  return saturated;
}

double FrequencyEstimator::rate(const FrequencyState& state, TimePoint now) const {
  if (!state.recorded) return 0.0;
  if (config_.mode == Mode::ForwardDecay) {
    return std::exp(state.log_mass - time_offset(now)) / config_.decay_tau_seconds;
  }
  const std::uint32_t buckets = config_.window_seconds / config_.bucket_seconds;
  const std::uint32_t e = std::max(epoch(now), state.last_epoch);
  if (e - state.last_epoch >= buckets) return 0.0;
  const std::uint32_t first = e + 1 >= buckets ? e + 1 - buckets : 0;
  std::uint64_t count = 0;
  for (std::uint32_t ep = first; ep <= state.last_epoch; ++ep) count += state.buckets[ep % buckets];
  return static_cast<double>(count) / static_cast<double>(config_.window_seconds);
}

double FrequencyEstimator::log_rate_key(const FrequencyState& state, TimePoint now) const {
  if (config_.mode == Mode::ForwardDecay) {
    return state.recorded ? state.log_mass - std::log(config_.decay_tau_seconds) : kNegInf;
  }
  const double r = rate(state, now);
  return r > 0.0 ? std::log(r) : kNegInf;
}

MissCostEstimator::MissCostEstimator(const MissCostConfig& config) : config_(config) {}

void MissCostEstimator::observe(CostState& state, double observed_ms) const {
  const double x = std::clamp(observed_ms, config_.min_ms, config_.max_ms);
  state.ewma_ms =
      state.samples == 0 ? x : config_.ewma_alpha * x + (1.0 - config_.ewma_alpha) * state.ewma_ms;
  if (state.samples != std::numeric_limits<std::uint32_t>::max()) ++state.samples;
}

double MissCostEstimator::estimate_ms(const CostState& state) const {
  if (state.samples == 0) return config_.default_ms;
  if (state.samples < config_.min_samples) return std::min(state.ewma_ms, config_.default_ms);
  return state.ewma_ms;
}

}  // namespace clache
