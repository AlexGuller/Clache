// Configuration structs (Spec §12.1, §18). Defaults are experimental
// placeholders; Phase 2 sets the documented defaults.
//
// Parsing (YAML subset + --set overrides) and validate() are implemented on
// feat/config-parser; this header only fixes the shape.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace clache {

struct FrequencyConfig {
  enum class Mode : std::uint8_t { RollingBuckets, ForwardDecay };
  Mode mode = Mode::RollingBuckets;
  std::uint32_t window_seconds = 300;  // W
  std::uint32_t bucket_seconds = 60;   // b; W % b == 0, W / b in [1, kMaxBuckets]
  double decay_tau_seconds = 300.0;    // tau, forward-decay mode only
};

struct MissCostConfig {
  double ewma_alpha = 0.20;       // (0, 1]
  double min_ms = 0.01;           // > 0; samples clamped to [min_ms, max_ms]
  double max_ms = 30000.0;        // >= min_ms
  double default_ms = 0.01;       // estimate before any sample
  std::uint32_t min_samples = 1;  // below this, estimate = min(ewma, default_ms)
};

struct ValueConfig {
  double size_exponent = 1.0;  // alpha in V = F*C / S^alpha; [0, 2]
};

struct AdmissionConfig {
  std::uint32_t min_observations = 1;  // >= 1; 1 = admit on first miss
  std::uint32_t max_victims = 0;       // 0 = unlimited (Clache only)
};

struct ObservationConfig {
  std::uint64_t max_entries = 1'000'000;  // 0 disables ghosts
  std::uint32_t ttl_seconds = 1200;
};

struct TelemetryConfig {
  double decision_sample_rate = 0.001;  // [0, 1]
  bool sample_hits = false;
};

struct EngineConfig {
  std::uint64_t capacity_bytes = 0;    // required, > 0
  std::uint64_t max_object_bytes = 0;  // 0 = capacity_bytes
  FrequencyConfig frequency;
  MissCostConfig miss_cost;
  ObservationConfig observation;
  AdmissionConfig admission;
  ValueConfig value;
  TelemetryConfig telemetry;
  std::uint64_t seed = 0;
};

// ---- Per-policy settings; each policy ignores the others. -------------------

struct ClachePolicyConfig {
  enum class AdmissionRule : std::uint8_t { Strict, Always };
  bool use_cost = true;
  AdmissionRule admission_rule = AdmissionRule::Strict;
};

struct GdsfPolicyConfig {
  enum class FrequencySource : std::uint8_t { ResidentCount, EngineWindow };
  FrequencySource frequency_source = FrequencySource::ResidentCount;
  double size_exponent = 1.0;
};

struct TinyLfuPolicyConfig {
  double window_fraction = 0.01;
  double protected_fraction = 0.80;  // of main
};

struct S3FifoPolicyConfig {
  double small_fraction = 0.10;
  std::uint32_t move_to_main_threshold = 1;
};

struct RedisPolicyConfig {
  std::uint32_t samples = 5;
  std::uint32_t pool_size = 16;
  std::uint32_t lfu_log_factor = 10;
  std::uint32_t lfu_decay_time_min = 1;
};

struct PolicyConfig {
  ClachePolicyConfig clache;
  GdsfPolicyConfig gdsf;
  TinyLfuPolicyConfig tinylfu;
  S3FifoPolicyConfig s3fifo;
  RedisPolicyConfig redis;
};

// Returns every validation error (empty = valid). Implemented on
// feat/config-parser (Spec §18.3).
std::vector<std::string> validate(const EngineConfig& config);
std::vector<std::string> validate(const PolicyConfig& config);

}  // namespace clache
