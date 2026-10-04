#include <algorithm>
#include <cmath>
#include <limits>

#include "clache/check.h"
#include "clache/estimators.h"

namespace clache {

FrequencyEstimator::FrequencyEstimator(const FrequencyConfig& config) : config_(config) {
  if (config_.mode == Mode::RollingBuckets) {
    CLACHE_REQUIRE(config_.bucket_seconds > 0, "bucket_seconds must be positive");
    CLACHE_REQUIRE(config_.window_seconds > 0 &&
                       config_.window_seconds % config_.bucket_seconds == 0 &&
                       config_.window_seconds / config_.bucket_seconds <= kMaxBuckets,
                   "window must contain between one and eight whole buckets");
  } else {
    CLACHE_REQUIRE(std::isfinite(config_.decay_tau_seconds) && config_.decay_tau_seconds > 0,
                   "decay tau must be finite and positive");
  }
}

void FrequencyEstimator::set_origin(TimePoint t0) { t0_ = t0; }
TimePoint FrequencyEstimator::origin() const noexcept { return t0_; }

std::uint32_t FrequencyEstimator::epoch(TimePoint now) const {
  CLACHE_ASSERT(now >= t0_, "time precedes frequency origin");
  const auto elapsed = static_cast<std::uint64_t>(now.ns()) - static_cast<std::uint64_t>(t0_.ns());
  const auto result =
      elapsed / (static_cast<std::uint64_t>(config_.bucket_seconds) * 1'000'000'000);
  CLACHE_ASSERT(result <= std::numeric_limits<std::uint32_t>::max(), "frequency epoch overflow");
  return static_cast<std::uint32_t>(result);
}

double FrequencyEstimator::time_offset(TimePoint now) const {
  if (config_.mode == Mode::RollingBuckets) return 0.0;
  CLACHE_ASSERT(now >= t0_, "time precedes frequency origin");
  const auto elapsed = static_cast<std::uint64_t>(now.ns()) - static_cast<std::uint64_t>(t0_.ns());
  const double offset = static_cast<double>(elapsed) * 1e-9 / config_.decay_tau_seconds;
  CLACHE_ASSERT(offset <= 1e6, "forward decay requires rebasing after one million tau");
  return offset;
}

bool FrequencyEstimator::record(FrequencyState& state, TimePoint now) const {
  if (config_.mode == Mode::ForwardDecay) {
    const double offset = time_offset(now);
    if (!state.recorded) {
      state.log_mass = offset;
    } else {
      state.log_mass = std::max(state.log_mass, offset) +
                       std::log1p(std::exp(-std::abs(state.log_mass - offset)));
    }
    state.recorded = true;
    return false;
  }
  const auto current = epoch(now);
  const auto count = config_.window_seconds / config_.bucket_seconds;
  CLACHE_ASSERT(!state.recorded || current >= state.last_epoch, "frequency time decreased");
  if (!state.recorded || current - state.last_epoch >= count) {
    state.buckets.fill(0);
  } else {
    const auto gap = current - state.last_epoch;
    for (std::uint32_t i = 1; i <= gap; ++i) {
      state.buckets[(state.last_epoch + i) % count] = 0;
    }
  }
  auto& bucket = state.buckets[current % count];
  const bool saturated = bucket == std::numeric_limits<std::uint16_t>::max();
  if (!saturated) ++bucket;
  state.last_epoch = current;
  state.recorded = true;
  return saturated;
}

double FrequencyEstimator::rate(const FrequencyState& state, TimePoint now) const {
  if (!state.recorded) return 0.0;
  if (config_.mode == Mode::ForwardDecay) {
    return std::exp(state.log_mass - time_offset(now)) / config_.decay_tau_seconds;
  }
  const auto current = epoch(now);
  const auto count = config_.window_seconds / config_.bucket_seconds;
  CLACHE_ASSERT(current >= state.last_epoch, "frequency time decreased");
  const auto gap = current - state.last_epoch;
  if (gap >= count) return 0.0;
  std::uint32_t total = 0;
  for (std::uint32_t age = gap; age < count && age <= current; ++age) {
    total += state.buckets[(current - age) % count];
  }
  return static_cast<double>(total) / config_.window_seconds;
}

double FrequencyEstimator::log_rate_key(const FrequencyState& state, TimePoint now) const {
  if (!state.recorded) return -std::numeric_limits<double>::infinity();
  if (config_.mode == Mode::ForwardDecay) {
    return state.log_mass - std::log(config_.decay_tau_seconds);
  }
  return std::log(rate(state, now));
}

}  // namespace clache
