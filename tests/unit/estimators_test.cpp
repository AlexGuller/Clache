#include "clache/estimators.h"

#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace clache {
namespace {
TimePoint Seconds(std::int64_t seconds) { return TimePoint::from_ns(seconds * 1'000'000'000); }

TEST(FrequencyBuckets, FirstTouchOriginBoundariesAndPureReads) {
  FrequencyEstimator estimator(FrequencyConfig{});
  estimator.set_origin(Seconds(1000));
  EXPECT_EQ(estimator.origin(), Seconds(1000));
  FrequencyState state;
  EXPECT_EQ(estimator.rate(state, Seconds(1000)), 0);
  EXPECT_EQ(estimator.log_rate_key(state, Seconds(1000)), -std::numeric_limits<double>::infinity());
  EXPECT_FALSE(estimator.record(state, Seconds(1000)));
  EXPECT_DOUBLE_EQ(estimator.rate(state, Seconds(1059)), 1.0 / 300);
  EXPECT_EQ(estimator.epoch(TimePoint::from_ns(1060'000'000'000 - 1)), 0U);
  EXPECT_EQ(estimator.epoch(Seconds(1060)), 1U);
  estimator.record(state, Seconds(1060));
  estimator.record(state, Seconds(1240));
  const FrequencyState before = state;
  EXPECT_DOUBLE_EQ(estimator.rate(state, Seconds(1299)), 3.0 / 300);
  EXPECT_DOUBLE_EQ(estimator.rate(state, Seconds(1300)), 2.0 / 300);
  EXPECT_DOUBLE_EQ(estimator.rate(state, Seconds(1360)), 1.0 / 300);
  EXPECT_DOUBLE_EQ(estimator.rate(state, Seconds(1540)), 0);
  EXPECT_EQ(state.buckets, before.buckets);
  EXPECT_EQ(state.last_epoch, before.last_epoch);
  EXPECT_EQ(state.recorded, before.recorded);
  EXPECT_EQ(state.log_mass, before.log_mass);
  estimator.record(state, Seconds(1540));
  EXPECT_DOUBLE_EQ(estimator.rate(state, Seconds(1540)), 1.0 / 300);
  EXPECT_DOUBLE_EQ(estimator.log_rate_key(state, Seconds(1540)), std::log(1.0 / 300));
  EXPECT_DOUBLE_EQ(estimator.time_offset(Seconds(1540)), 0);
}

TEST(FrequencyBuckets, SaturatesAndClearsReusedBuckets) {
  FrequencyConfig config;
  config.window_seconds = 2;
  config.bucket_seconds = 1;
  FrequencyEstimator estimator(config);
  FrequencyState state;
  for (std::uint32_t i = 0; i < 65535; ++i) EXPECT_FALSE(estimator.record(state, Seconds(0)));
  EXPECT_TRUE(estimator.record(state, Seconds(0)));
  EXPECT_DOUBLE_EQ(estimator.rate(state, Seconds(0)), 65535.0 / 2);
  estimator.record(state, Seconds(1));
  estimator.record(state, Seconds(2));
  EXPECT_DOUBLE_EQ(estimator.rate(state, Seconds(2)), 1);
  estimator.record(state, Seconds(8));
  EXPECT_DOUBLE_EQ(estimator.rate(state, Seconds(8)), 0.5);
}

TEST(FrequencyBuckets, EightBucketRingWithNegativeOrigin) {
  FrequencyConfig config;
  config.window_seconds = 8;
  config.bucket_seconds = 1;
  FrequencyEstimator estimator(config);
  estimator.set_origin(Seconds(-10));
  FrequencyState state;
  for (std::int64_t i = 0; i < 8; ++i) estimator.record(state, Seconds(-10 + i));
  EXPECT_DOUBLE_EQ(estimator.rate(state, Seconds(-3)), 1);
  EXPECT_DOUBLE_EQ(estimator.rate(state, Seconds(-2)), 7.0 / 8);
  estimator.record(state, Seconds(-2));
  EXPECT_DOUBLE_EQ(estimator.rate(state, Seconds(-2)), 1);
  EXPECT_DOUBLE_EQ(estimator.rate(state, Seconds(6)), 0);
}

TEST(FrequencyDecay, MatchesDirectSumAndRanksAtRandomTimes) {
  FrequencyConfig config;
  config.mode = FrequencyConfig::Mode::ForwardDecay;
  config.decay_tau_seconds = 10;
  FrequencyEstimator estimator(config);
  estimator.set_origin(Seconds(100));
  std::mt19937 rng(42);
  std::vector<FrequencyState> states(20);
  std::vector<std::vector<std::int64_t>> events(20);
  for (std::int64_t now = 100; now < 500; ++now) {
    const auto key = rng() % states.size();
    EXPECT_FALSE(estimator.record(states[key], Seconds(now)));
    events[key].push_back(now);
    for (std::size_t i = 0; i < states.size(); ++i) {
      double sum = 0;
      for (const auto event : events[i]) sum += std::exp(-static_cast<double>(now - event) / 10);
      EXPECT_NEAR(estimator.rate(states[i], Seconds(now)), sum / 10, 1e-13);
      if (!events[i].empty()) {
        EXPECT_NEAR(
            estimator.log_rate_key(states[i], Seconds(now)) - estimator.time_offset(Seconds(now)),
            std::log(sum / 10), 1e-13);
      }
      const auto other = (i + 1) % states.size();
      EXPECT_EQ(
          estimator.rate(states[i], Seconds(now)) < estimator.rate(states[other], Seconds(now)),
          estimator.log_rate_key(states[i], Seconds(now)) <
              estimator.log_rate_key(states[other], Seconds(now)));
    }
  }
}

TEST(FrequencyDecay, FirstRecordAndLargeOffsetsRemainFinite) {
  FrequencyConfig config;
  config.mode = FrequencyConfig::Mode::ForwardDecay;
  FrequencyEstimator estimator(config);
  FrequencyState state;
  EXPECT_EQ(estimator.log_rate_key(state, Seconds(0)), -std::numeric_limits<double>::infinity());
  estimator.record(state, Seconds(30 * 86400));
  EXPECT_DOUBLE_EQ(state.log_mass, 8640);
  estimator.record(state, Seconds(30 * 86400));
  EXPECT_NEAR(estimator.rate(state, Seconds(30 * 86400)), 2.0 / 300, 1e-14);
  const double key = estimator.log_rate_key(state, Seconds(30 * 86400));
  EXPECT_EQ(key, estimator.log_rate_key(state, Seconds(31 * 86400)));
}

TEST(MissCost, InitializesSmoothsClampsAndLimitsPrior) {
  MissCostConfig config;
  config.min_ms = 1;
  config.max_ms = 100;
  config.default_ms = 5;
  config.min_samples = 3;
  config.ewma_alpha = 0.25;
  MissCostEstimator estimator(config);
  CostState state;
  EXPECT_EQ(estimator.estimate_ms(state), 5);
  estimator.observe(state, 20);
  EXPECT_EQ(state.ewma_ms, 20);
  EXPECT_EQ(estimator.estimate_ms(state), 5);
  estimator.observe(state, 40);
  EXPECT_EQ(state.ewma_ms, 25);
  EXPECT_EQ(estimator.estimate_ms(state), 5);
  estimator.observe(state, 1000);
  EXPECT_EQ(estimator.estimate_ms(state), 43.75);
  CostState low;
  estimator.observe(low, -10);
  EXPECT_EQ(estimator.estimate_ms(low), 1);
  CostState high;
  estimator.observe(high, std::numeric_limits<double>::infinity());
  EXPECT_EQ(high.ewma_ms, 100);
  high.samples = std::numeric_limits<std::uint32_t>::max();
  estimator.observe(high, 100);
  EXPECT_EQ(high.samples, std::numeric_limits<std::uint32_t>::max());
}
}  // namespace
}  // namespace clache
