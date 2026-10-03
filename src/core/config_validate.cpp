#include <cmath>
#include <string>
#include <vector>

#include "clache/config.h"
#include "clache/estimators.h"

namespace clache {
namespace {
bool between(double value, double low, double high) {
  return std::isfinite(value) && value >= low && value <= high;
}
}  // namespace

std::vector<std::string> validate(const EngineConfig& c) {
  std::vector<std::string> errors;
  const auto require = [&](bool valid, const char* key, const char* message) {
    if (!valid) errors.push_back(std::string("clache.") + key + ": " + message);
  };
  require(c.capacity_bytes > 0, "capacity_bytes", "must be greater than zero");
  require(c.max_object_bytes == 0 || c.max_object_bytes <= c.capacity_bytes, "max_object_bytes",
          "must be zero or no greater than capacity_bytes");
  const auto& f = c.frequency;
  require(f.mode == FrequencyConfig::Mode::RollingBuckets ||
              f.mode == FrequencyConfig::Mode::ForwardDecay,
          "frequency.mode", "invalid frequency mode");
  require(f.window_seconds > 0, "frequency.window_seconds", "must be greater than zero");
  require(f.bucket_seconds > 0, "frequency.bucket_seconds", "must be greater than zero");
  if (f.bucket_seconds > 0 && f.window_seconds > 0) {
    require(f.window_seconds % f.bucket_seconds == 0, "frequency.bucket_seconds",
            "window_seconds must be a multiple of bucket_seconds");
    const auto buckets = f.window_seconds / f.bucket_seconds;
    require(buckets >= 1 && buckets <= kMaxBuckets, "frequency.window_seconds",
            "window_seconds / bucket_seconds must be in [1, kMaxBuckets]");
  }
  require(std::isfinite(f.decay_tau_seconds) && f.decay_tau_seconds > 0,
          "frequency.decay_tau_seconds", "must be finite and greater than zero");
  const auto& m = c.miss_cost;
  require(between(m.ewma_alpha, 0, 1) && m.ewma_alpha > 0, "miss_cost.ewma_alpha",
          "must be in (0, 1]");
  require(std::isfinite(m.min_ms) && m.min_ms > 0, "miss_cost.min_ms",
          "must be finite and greater than zero");
  require(std::isfinite(m.max_ms) && m.max_ms >= m.min_ms, "miss_cost.max_ms",
          "must be finite and at least min_ms");
  require(std::isfinite(m.default_ms) && m.default_ms >= 0, "miss_cost.default_ms",
          "must be finite and nonnegative");
  require(m.min_samples >= 1, "miss_cost.min_samples", "must be at least one");
  require(between(c.value.size_exponent, 0, 2), "value.size_exponent", "must be in [0, 2]");
  require(c.admission.min_observations >= 1, "admission.min_observations", "must be at least one");
  require(between(c.telemetry.decision_sample_rate, 0, 1), "telemetry.decision_sample_rate",
          "must be in [0, 1]");
  return errors;
}

std::vector<std::string> validate(const PolicyConfig& c) {
  std::vector<std::string> errors;
  const auto require = [&](bool valid, const char* key, const char* message) {
    if (!valid) errors.push_back(std::string("policy.") + key + ": " + message);
  };
  require(c.clache.admission_rule == ClachePolicyConfig::AdmissionRule::Strict ||
              c.clache.admission_rule == ClachePolicyConfig::AdmissionRule::Always,
          "clache.admission_rule", "invalid admission rule");
  require(c.gdsf.frequency_source == GdsfPolicyConfig::FrequencySource::ResidentCount ||
              c.gdsf.frequency_source == GdsfPolicyConfig::FrequencySource::EngineWindow,
          "gdsf.frequency_source", "invalid frequency source");
  require(between(c.gdsf.size_exponent, 0, 2), "gdsf.size_exponent", "must be in [0, 2]");
  const auto fraction = [&](double value, const char* key) {
    require(std::isfinite(value) && value > 0 && value < 1, key, "must be in (0, 1)");
  };
  fraction(c.tinylfu.window_fraction, "tinylfu.window_fraction");
  fraction(c.tinylfu.protected_fraction, "tinylfu.protected_fraction");
  fraction(c.s3fifo.small_fraction, "s3fifo.small_fraction");
  require(c.redis.samples >= 1, "redis.samples", "must be at least one");
  require(c.redis.pool_size >= 1, "redis.pool_size", "must be at least one");
  return errors;
}
}  // namespace clache
