#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "clache/config.h"

namespace clache {
namespace {
TEST(ConfigValidate, DefaultsNeedCapacityAndAllowDocumentedZeroSentinels) {
  EngineConfig c;
  EXPECT_EQ(validate(c).size(), 1U);
  c.capacity_bytes = 1024;
  c.observation.max_entries = 0;
  c.observation.ttl_seconds = 0;
  c.admission.max_victims = 0;
  c.miss_cost.default_ms = 0;
  c.telemetry.decision_sample_rate = 0;
  EXPECT_TRUE(validate(c).empty());
  c.max_object_bytes = c.capacity_bytes;
  c.miss_cost.ewma_alpha = 1;
  c.miss_cost.max_ms = c.miss_cost.min_ms;
  c.value.size_exponent = 2;
  c.telemetry.decision_sample_rate = 1;
  c.frequency.window_seconds = 480;
  EXPECT_TRUE(validate(c).empty());
  c.frequency.window_seconds = 60;
  c.value.size_exponent = 0;
  EXPECT_TRUE(validate(c).empty());
  EXPECT_TRUE(validate(PolicyConfig{}).empty());
}

TEST(ConfigValidate, EveryEngineConstraintNamesItsKey) {
  using Case = std::pair<std::string, std::function<void(EngineConfig&)>>;
  const std::vector<Case> cases = {
      {"capacity_bytes", [](auto& c) { c.capacity_bytes = 0; }},
      {"max_object_bytes", [](auto& c) { c.max_object_bytes = 1025; }},
      {"frequency.mode",
       [](auto& c) { c.frequency.mode = static_cast<FrequencyConfig::Mode>(99); }},
      {"frequency.window_seconds", [](auto& c) { c.frequency.window_seconds = 0; }},
      {"frequency.bucket_seconds", [](auto& c) { c.frequency.bucket_seconds = 0; }},
      {"frequency.bucket_seconds", [](auto& c) { c.frequency.bucket_seconds = 70; }},
      {"frequency.window_seconds", [](auto& c) { c.frequency.window_seconds = 540; }},
      {"frequency.window_seconds", [](auto& c) { c.frequency.bucket_seconds = 600; }},
      {"frequency.decay_tau_seconds", [](auto& c) { c.frequency.decay_tau_seconds = 0; }},
      {"miss_cost.ewma_alpha", [](auto& c) { c.miss_cost.ewma_alpha = 0; }},
      {"miss_cost.ewma_alpha", [](auto& c) { c.miss_cost.ewma_alpha = 1.01; }},
      {"miss_cost.min_ms", [](auto& c) { c.miss_cost.min_ms = 0; }},
      {"miss_cost.max_ms", [](auto& c) { c.miss_cost.max_ms = 0; }},
      {"miss_cost.default_ms", [](auto& c) { c.miss_cost.default_ms = -1; }},
      {"miss_cost.min_samples", [](auto& c) { c.miss_cost.min_samples = 0; }},
      {"value.size_exponent", [](auto& c) { c.value.size_exponent = -0.1; }},
      {"value.size_exponent", [](auto& c) { c.value.size_exponent = 2.1; }},
      {"admission.min_observations", [](auto& c) { c.admission.min_observations = 0; }},
      {"telemetry.decision_sample_rate", [](auto& c) { c.telemetry.decision_sample_rate = -0.1; }},
      {"telemetry.decision_sample_rate", [](auto& c) { c.telemetry.decision_sample_rate = 1.1; }}};
  for (const auto& [key, change] : cases) {
    SCOPED_TRACE(key);
    EngineConfig c;
    c.capacity_bytes = 1024;
    change(c);
    const auto errors = validate(c);
    bool found = false;
    for (const auto& error : errors) found = found || error.starts_with("clache." + key + ":");
    EXPECT_TRUE(found);
  }
}

TEST(ConfigValidate, AllFloatingPointFieldsRejectNonfiniteValues) {
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    EngineConfig c;
    c.capacity_bytes = 1024;
    c.frequency.decay_tau_seconds = bad;
    c.miss_cost.ewma_alpha = bad;
    c.miss_cost.min_ms = bad;
    c.miss_cost.max_ms = bad;
    c.miss_cost.default_ms = bad;
    c.value.size_exponent = bad;
    c.telemetry.decision_sample_rate = bad;
    EXPECT_EQ(validate(c).size(), 7U);
    PolicyConfig p;
    p.gdsf.size_exponent = bad;
    p.tinylfu.window_fraction = bad;
    p.tinylfu.protected_fraction = bad;
    p.s3fifo.small_fraction = bad;
    EXPECT_EQ(validate(p).size(), 4U);
  }
}

TEST(ConfigValidate, EveryPolicyConstraintAndAllErrorsTogether) {
  PolicyConfig p;
  p.clache.admission_rule = static_cast<ClachePolicyConfig::AdmissionRule>(99);
  p.gdsf.frequency_source = static_cast<GdsfPolicyConfig::FrequencySource>(99);
  p.gdsf.size_exponent = -1;
  p.tinylfu.window_fraction = 0;
  p.tinylfu.protected_fraction = 1;
  p.s3fifo.small_fraction = -1;
  p.redis.samples = 0;
  p.redis.pool_size = 0;
  const auto errors = validate(p);
  ASSERT_EQ(errors.size(), 8U);
  const std::vector<std::string> keys = {"clache.admission_rule",
                                         "gdsf.frequency_source",
                                         "gdsf.size_exponent",
                                         "tinylfu.window_fraction",
                                         "tinylfu.protected_fraction",
                                         "s3fifo.small_fraction",
                                         "redis.samples",
                                         "redis.pool_size"};
  for (std::size_t i = 0; i < keys.size(); ++i) {
    EXPECT_TRUE(errors[i].starts_with("policy." + keys[i] + ":"));
  }
  for (double invalid : {-0.1, 0.0, 1.0, 1.1}) {
    PolicyConfig fractions;
    fractions.tinylfu.window_fraction = invalid;
    fractions.tinylfu.protected_fraction = invalid;
    fractions.s3fifo.small_fraction = invalid;
    EXPECT_EQ(validate(fractions).size(), 3U);
  }
  p = PolicyConfig{};
  p.gdsf.size_exponent = 2.01;
  EXPECT_EQ(validate(p).size(), 1U);
  p.gdsf.size_exponent = 2;
  EXPECT_TRUE(validate(p).empty());
  p.gdsf.size_exponent = 0;
  EXPECT_TRUE(validate(p).empty());
}
}  // namespace
}  // namespace clache
