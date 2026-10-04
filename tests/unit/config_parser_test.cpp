#include "clache/config_parser.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "clache/hash.h"

namespace clache {
namespace {

TEST(ConfigParser, EveryFieldAndFlowMaps) {
  ClacheConfig c;
  apply_config_text(c, R"(
# all settings, deliberately different from defaults
clache:
  capacity_bytes: 2GiB # inline comment
  max_object_bytes: 4MiB
  seed: 18446744073709551615
  frequency:
    mode: decay
    window_seconds: 240
    bucket_seconds: 30
    decay_tau_seconds: 123.25
  miss_cost:
    ewma_alpha: 0.5
    min_ms: 0.1
    max_ms: 5000
    default_ms: 2.5
    min_samples: 3
  value:
    size_exponent: 1.5
  admission:
    min_observations: 2
    max_victims: 12
  observation_table:
    max_entries: 12345
    ttl_seconds: 500
  telemetry:
    decision_sample_rate: 0.2
    sample_hits: true
policy:
  clache: { use_cost: false, admission_rule: always }
  gdsf: { frequency_source: engine_window, size_exponent: 0.5 }
  tinylfu: { window_fraction: 0.2, protected_fraction: 0.7 }
  s3fifo: { small_fraction: 0.3, move_to_main_threshold: 2 }
  redis: { samples: 9, pool_size: 20, lfu_log_factor: 5, lfu_decay_time_min: 3 }
)");
  EXPECT_EQ(c.engine.capacity_bytes, 2ULL * 1024 * 1024 * 1024);
  EXPECT_EQ(c.engine.max_object_bytes, 4ULL * 1024 * 1024);
  EXPECT_EQ(c.engine.seed, std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(c.engine.frequency.mode, FrequencyConfig::Mode::ForwardDecay);
  EXPECT_EQ(c.engine.frequency.window_seconds, 240U);
  EXPECT_EQ(c.engine.frequency.bucket_seconds, 30U);
  EXPECT_EQ(c.engine.frequency.decay_tau_seconds, 123.25);
  EXPECT_EQ(c.engine.miss_cost.ewma_alpha, 0.5);
  EXPECT_EQ(c.engine.miss_cost.min_ms, 0.1);
  EXPECT_EQ(c.engine.miss_cost.max_ms, 5000);
  EXPECT_EQ(c.engine.miss_cost.default_ms, 2.5);
  EXPECT_EQ(c.engine.miss_cost.min_samples, 3U);
  EXPECT_EQ(c.engine.value.size_exponent, 1.5);
  EXPECT_EQ(c.engine.admission.min_observations, 2U);
  EXPECT_EQ(c.engine.admission.max_victims, 12U);
  EXPECT_EQ(c.engine.observation.max_entries, 12345U);
  EXPECT_EQ(c.engine.observation.ttl_seconds, 500U);
  EXPECT_EQ(c.engine.telemetry.decision_sample_rate, 0.2);
  EXPECT_TRUE(c.engine.telemetry.sample_hits);
  EXPECT_FALSE(c.policy.clache.use_cost);
  EXPECT_EQ(c.policy.clache.admission_rule, ClachePolicyConfig::AdmissionRule::Always);
  EXPECT_EQ(c.policy.gdsf.frequency_source, GdsfPolicyConfig::FrequencySource::EngineWindow);
  EXPECT_EQ(c.policy.gdsf.size_exponent, 0.5);
  EXPECT_EQ(c.policy.tinylfu.window_fraction, 0.2);
  EXPECT_EQ(c.policy.tinylfu.protected_fraction, 0.7);
  EXPECT_EQ(c.policy.s3fifo.small_fraction, 0.3);
  EXPECT_EQ(c.policy.s3fifo.move_to_main_threshold, 2U);
  EXPECT_EQ(c.policy.redis.samples, 9U);
  EXPECT_EQ(c.policy.redis.pool_size, 20U);
  EXPECT_EQ(c.policy.redis.lfu_log_factor, 5U);
  EXPECT_EQ(c.policy.redis.lfu_decay_time_min, 3U);
  EXPECT_TRUE(validate(c.engine).empty());
  EXPECT_TRUE(validate(c.policy).empty());
}

TEST(ConfigParser, SourceLinesAndAtomicOverlay) {
  ClacheConfig c;
  try {
    apply_config_text(c, "clache:\n  seed: 42\n  surprise: 1\n", "settings.yaml");
    FAIL() << "expected error";
  } catch (const ConfigError& e) {
    EXPECT_EQ(e.source(), "settings.yaml");
    EXPECT_EQ(e.line(), 3U);
    EXPECT_NE(std::string(e.what()).find("settings.yaml:3: clache.surprise"), std::string::npos);
  }
  EXPECT_EQ(c.engine.seed, 0U);
}

TEST(ConfigParser, DuplicateErrorsIdentifyTheSecondOccurrence) {
  for (const auto* text :
       {"clache:\n  seed: 1\n  seed: 2", "policy:\n  redis:\n    samples: 1\n    samples: 2"}) {
    ClacheConfig c;
    const std::string input(text);
    try {
      apply_config_text(c, input, "duplicate.yaml");
      FAIL() << "expected duplicate error";
    } catch (const ConfigError& e) {
      EXPECT_EQ(e.line(),
                static_cast<std::size_t>(std::count(input.begin(), input.end(), '\n')) + 1);
      EXPECT_NE(std::string(e.what()).find("duplicate key"), std::string::npos);
    }
  }
}

TEST(ConfigParser, RejectsInvalidSyntaxAndDuplicates) {
  const std::vector<std::string> invalid = {"clache:\n seed: 1",
                                            "clache:\n    seed: 1",
                                            "clache:\n\tseed: 1",
                                            "clache:\n  - seed: 1",
                                            "clache: &anchor",
                                            "clache:\n  seed: |\n    1",
                                            "clache:\n  seed: 1\n  seed: 2",
                                            "clache:\nclache:\n",
                                            "unknown:\n",
                                            "clache:\n  seed:\n",
                                            "clache:\n  seed: 1\n    extra: 2",
                                            "clache: { seed: 1, seed: 2 }",
                                            "policy: { redis: { samples: 3 } }",
                                            "policy:\n  redis: { samples: 3, }",
                                            "policy:\n  redis: { samples: 3",
                                            "policy:\n  redis: { samples 3 }",
                                            "clache:\n  seed: *anchor",
                                            "clache:\n  seed: [1, 2]",
                                            "clache.seed: 1",
                                            "clache:\n  seed: >",
                                            "clache:\n  : 1",
                                            "clache:\n  seed: { unknown: 1 }"};
  for (const auto& text : invalid) {
    SCOPED_TRACE(text);
    ClacheConfig c;
    EXPECT_THROW(apply_config_text(c, text), ConfigError);
  }
}

TEST(ConfigParser, RejectsBadTypesAndNumericOverflow) {
  const std::vector<std::string> invalid = {"clache.seed=-1",
                                            "clache.seed=1.5",
                                            "clache.seed=18446744073709551616",
                                            "clache.frequency.window_seconds=4294967296",
                                            "clache.frequency.window_seconds=10MiB",
                                            "clache.telemetry.sample_hits=1",
                                            "clache.telemetry.sample_hits=True",
                                            "clache.frequency.mode=invalid",
                                            "policy.clache.admission_rule=invalid",
                                            "policy.gdsf.frequency_source=invalid",
                                            "clache.miss_cost.min_ms=nan",
                                            "clache.miss_cost.min_ms=inf",
                                            "clache.miss_cost.min_ms=1e999",
                                            "clache.seed=",
                                            "clache.capacity_bytes=16GiBjunk",
                                            "clache.capacity_bytes=-1",
                                            "clache.miss_cost.min_ms=1.0junk",
                                            "clache.seed=0xFF",
                                            "not_a_key=1",
                                            "missing_equals"};
  for (const auto& assignment : invalid) {
    SCOPED_TRACE(assignment);
    ClacheConfig c;
    try {
      apply_override(c, assignment);
      FAIL() << "expected error";
    } catch (const ConfigError& e) {
      EXPECT_EQ(e.source(), "--set");
      EXPECT_EQ(e.line(), 0U);
    }
  }
}

TEST(ConfigParser, SizeSuffixesAndOverflow) {
  EXPECT_EQ(parse_size("1073741824"), 1073741824ULL);
  EXPECT_EQ(parse_size("64MiB"), 67108864ULL);
  EXPECT_EQ(parse_size("1GiB"), 1073741824ULL);
  EXPECT_EQ(parse_size("512KiB"), 524288ULL);
  EXPECT_EQ(parse_size("64MB"), 64000000ULL);
  EXPECT_EQ(parse_size("1GB"), 1000000000ULL);
  EXPECT_EQ(parse_size("2KB"), 2000ULL);
  EXPECT_EQ(parse_size(" 1B "), 1U);
  EXPECT_EQ(parse_size("0"), 0U);
  EXPECT_EQ(parse_size("18446744073709551615"), std::numeric_limits<std::uint64_t>::max());
  for (const auto* text : {"", "MiB", "-1", "1.5MiB", "1 MiB", "1TiB", "18446744073709551616",
                           "18014398509481984KiB"}) {
    EXPECT_THROW(parse_size(text), std::invalid_argument) << text;
  }
}

TEST(ConfigParser, FileAndOverridePrecedence) {
  const auto root = std::filesystem::temp_directory_path();
  std::filesystem::path directory;
  for (std::size_t i = 0;; ++i) {
    directory = root / ("clache_config_parser_test_" + std::to_string(i));
    if (std::filesystem::create_directory(directory)) break;
  }
  const auto path = directory / "config.yaml";
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      std::filesystem::remove(path.parent_path(), ignored);
    }
  } cleanup{path};
  {
    std::ofstream out(path);
    out << "clache:\n  seed: 1\n";
  }
  ClacheConfig c;
  apply_config_file(c, path);
  EXPECT_EQ(c.engine.seed, 1U);
  apply_override(c, "clache.seed=2");
  apply_override(c, "clache.seed=3");
  EXPECT_EQ(c.engine.seed, 3U);
  EXPECT_EQ(c.engine.frequency.window_seconds, 300U);
  // Zero capacity is permitted during parsing, before the simulator supplies it.
  EXPECT_FALSE(validate(c.engine).empty());
  std::filesystem::remove(path);
  EXPECT_THROW(apply_config_file(c, path), ConfigError);
}

TEST(ConfigParser, CanonicalRoundTripAndFingerprintCoversEveryField) {
  ClacheConfig c;
  const auto canonical = canonical_config(c);
  std::istringstream lines(canonical);
  std::vector<std::string> keys;
  for (std::string line; std::getline(lines, line);) {
    const auto equal = line.find('=');
    const auto key = line.substr(0, equal);
    const auto old_value = line.substr(equal + 1);
    keys.push_back(key);
    std::string value = "2";
    if (old_value == "true")
      value = "false";
    else if (old_value == "false")
      value = "true";
    else if (old_value == "rolling_buckets")
      value = "forward_decay";
    else if (old_value == "strict")
      value = "always";
    else if (old_value == "resident_count")
      value = "engine_window";
    ClacheConfig changed = c;
    apply_override(changed, key + "=" + value);
    EXPECT_NE(config_fingerprint(changed), config_fingerprint(c)) << key;
    ClacheConfig round_trip;
    apply_config_text(round_trip, canonical_config(changed));
    EXPECT_EQ(canonical_config(round_trip), canonical_config(changed));
  }
  EXPECT_EQ(keys.size(), 31U);
  EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
  EXPECT_EQ(config_fingerprint(c), hash64(canonical));
  apply_override(c, "clache.frequency.mode=decay");
  EXPECT_NE(canonical_config(c).find("mode=forward_decay\n"), std::string::npos);
}

TEST(ConfigParser, DoubleSerializationAndInputAreLocaleIndependent) {
  ClacheConfig c;
  c.engine.miss_cost.min_ms = std::nextafter(0.1, 1.0);
  c.engine.frequency.decay_tau_seconds = 1e100;
  c.engine.value.size_exponent = -0.0;
  ClacheConfig restored;
  apply_config_text(restored, canonical_config(c));
  EXPECT_EQ(restored.engine.miss_cost.min_ms, c.engine.miss_cost.min_ms);
  EXPECT_EQ(restored.engine.frequency.decay_tau_seconds, 1e100);
  EXPECT_TRUE(std::signbit(restored.engine.value.size_exponent));
  EXPECT_THROW(apply_override(c, "clache.miss_cost.min_ms=0,1"), ConfigError);
}

}  // namespace
}  // namespace clache
