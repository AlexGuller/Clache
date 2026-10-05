#include <algorithm>
#include <cmath>
#include <limits>

#include "clache/check.h"
#include "clache/estimators.h"

namespace clache {

MissCostEstimator::MissCostEstimator(const MissCostConfig& config) : config_(config) {
  CLACHE_REQUIRE(
      std::isfinite(config_.ewma_alpha) && config_.ewma_alpha > 0 && config_.ewma_alpha <= 1,
      "EWMA alpha must be in (0, 1]");
  CLACHE_REQUIRE(std::isfinite(config_.min_ms) && config_.min_ms > 0 &&
                     std::isfinite(config_.max_ms) && config_.max_ms >= config_.min_ms,
                 "cost bounds must be finite, positive and ordered");
  CLACHE_REQUIRE(std::isfinite(config_.default_ms) && config_.default_ms >= 0,
                 "default cost must be finite and nonnegative");
}

void MissCostEstimator::observe(CostState& state, double observed_ms) const {
  CLACHE_REQUIRE(!std::isnan(observed_ms), "observed cost must not be NaN");
  const double sample = std::clamp(observed_ms, config_.min_ms, config_.max_ms);
  state.ewma_ms = state.samples == 0
                      ? sample
                      : config_.ewma_alpha * sample + (1.0 - config_.ewma_alpha) * state.ewma_ms;
  if (state.samples != std::numeric_limits<std::uint32_t>::max()) ++state.samples;
}

double MissCostEstimator::estimate_ms(const CostState& state) const {
  if (state.samples == 0) return config_.default_ms;
  return state.samples < config_.min_samples ? std::min(state.ewma_ms, config_.default_ms)
                                             : state.ewma_ms;
}

}  // namespace clache
